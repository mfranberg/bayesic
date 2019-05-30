#include <iostream>

#include <armadillo>

#include <plinkio/plinkio.h>

#include <gzstream/gzutil.hpp>
#include <besiq/io/covariates.hpp>
#include <cpp-argparse/OptionParser.h>

#include <plink/plink_file.hpp>
#include <besiq/model_matrix.hpp>
#include <glm/glm.hpp>
#include <glm/models/normal.hpp>
#include <glm/models/binomial.hpp>

#include "gene_environment.hpp"

using namespace arma;
using namespace optparse;

const std::string USAGE = "besiq-env [-c covariates.csv] [-m method] environment_file genotype_plink_prefix";
const std::string VERSION = "Besiq 0.0.1";
const std::string DESCRIPTION = "A tool for inferring variant-environment interactions.";
const std::string EPILOG = "";

void run_env(genotype_matrix_ptr genotypes, const arma::vec &phenotype, const arma::mat &cov, const arma::mat &env, const std::vector<std::string> &env_names, const arma::uvec &missing, glm_model &model, std::ostream &out)
{
    out << "variable\tbeta\tse_beta\tpvalue\tN\n";
    gene_environment ge( genotypes, env, phenotype, env_names, false );
    ge.impute_missing( );
    arma::vec cent_phenotype = ge.get_centered_phenotype( );
    arma::uvec indices = arma::zeros<arma::uvec>( 1 );
    size_t var = 0;
    arma::uvec minors = arma::zeros<arma::uvec>( genotypes->size( ) );
    for(int i = 0; i < genotypes->size( ) + env.n_cols; i++)
    {
        indices[0] = i;
        glm_info result;
        arma::mat mm = ge.get_active( indices );
        arma::vec beta = glm_fit( mm, cent_phenotype, missing, model, result );
        bool valid_minor = true;
        if( i < genotypes->size( ) )
        {
            minors[ i ] = ge.compute_num_minor( i );
            valid_minor = minors[ i ] >= 10;
        }

        if( valid_minor && result.converged && result.success )
        {
            out << ge.get_name(var) << "\t" <<
                beta[ 0 ] << "\t" <<
                result.se_beta[ 0 ] << "\t" <<
                result.p_value[ 0 ] << "\t" <<
                arma::sum( 1 - missing ) << "\n";
        }
        else
        {
            out << ge.get_name(var) << "\t" <<
                "NA" << "\t" <<
                "NA" << "\t" <<
                "NA" << "\t" <<
                arma::sum( 1 - missing ) << "\n";
        }
        var++;
    }

    indices = arma::zeros<arma::uvec>( 3 );
    for(int i = 0; i < genotypes->size( ); i++)
    {
        snp_row &row = genotypes->get_row( i );
        for(int j = 0; j < env.n_cols; j++)
        {
            size_t interaction_index = genotypes->size( ) + env.n_cols + j * genotypes->size( ) + i;
            indices[ 0 ] = i;
            indices[ 1 ] = genotypes->size( ) + j;
            indices[ 2 ] = interaction_index;

            arma::mat mm = ge.get_active( indices );
            glm_info result;
            arma::vec beta = glm_fit( mm, cent_phenotype, missing, model, result );

            if( minors[ i ] >= 10 && result.converged && result.success )
            {
                out << ge.get_name( interaction_index ) << "\t" <<
                    beta[ 2 ] << "\t" <<
                    result.se_beta[ 2 ] << "\t" <<
                    result.p_value[ 2 ] << "\t" <<
                    arma::sum( 1 - missing ) << "\n";
            }
            else
            {
                out << ge.get_name( interaction_index ) << "\t" <<
                    "NA" << "\t" <<
                    "NA" << "\t" <<
                    "NA" << "\t" <<
                    arma::sum( 1 - missing ) << "\n";
            }
        }
    }
}

int
main(int argc, char *argv[])
{
    OptionParser parser = OptionParser( ).usage( USAGE )
                                         .version( VERSION )
                                         .description( DESCRIPTION )
                                         .epilog( EPILOG );
    
    char const* const model_choices[] = { "binomial", "normal" };
    char const* const link_choices[] = { "logit", "logc", "odds", "identity", "log" };

    parser.add_option( "-m", "--model" ).choices( &model_choices[ 0 ], &model_choices[ 2 ] ).metavar( "model" ).help( "The model to use for the phenotype, 'binomial' or 'normal', default = 'binomial'." ).set_default( "normal" );
    parser.add_option( "-p", "--pheno" ).help( "Read phenotypes from this file instead of a plink file." );
    parser.add_option( "-n", "--mpheno" ).help( "Name of the phenotype to use." );
    parser.add_option( "-e", "--menv" ).help( "Name of the environment variable to use." );
    parser.add_option( "-c", "--cov" ).action( "store" ).type( "string" ).metavar( "filename" ).help( "Performs the analysis by including the covariates in this file." );
    parser.add_option( "-o", "--out" ).help( "The output file that will contain the results (binary)." );
    
    Values options = parser.parse_args( argc, argv );
    std::vector<std::string> args = parser.args( );
    if( args.size( ) != 2 )
    {
        std::cerr << "besiq: error: Genotypes or environment factor is missing." << std::endl;
        parser.print_help( );
        exit( 1 );
    }

    /* Read all genotypes */
    plink_file_ptr genotype_file = open_plink_file( args[ 1 ] );
    genotype_matrix_ptr genotypes = create_genotype_matrix( genotype_file );
    std::vector<std::string> locus_names = genotype_file->get_locus_names( );
 
    /* Read additional data  */
    arma::vec phenotype;
    arma::uvec missing;
    arma::mat cov;
    missing = zeros<uvec>( genotype_file->get_samples( ).size( ) );
    std::vector<std::string> order = genotype_file->get_sample_iids( );
    if( options.is_set( "pheno" ) )
    {
        std::ifstream phenotype_file( options[ "pheno" ].c_str( ) );
        phenotype = parse_phenotypes( phenotype_file, missing, order, options[ "mpheno" ] );
    }
    else
    {
        phenotype = create_phenotype_vector( genotype_file->get_samples( ), missing );
    }
    phenotype.elem( arma::find_nonfinite( phenotype ) ).zeros( );

    /* Read covariates */
    if( options.is_set( "cov" ) )
    {
        std::ifstream covariate_file( options[ "cov" ].c_str( ) );
        cov = parse_covariate_matrix( covariate_file, missing, order );
    }

    /* Read environment factor */
    std::vector<std::string> env_names;
    std::ifstream environment_file( args[ 0 ].c_str( ) );
    arma::uvec env_missing = arma::zeros<arma::uvec>( missing.n_elem );
    arma::mat E = parse_env( environment_file, env_missing, order, &env_names, options[ "menv" ] );

    /* Open output stream */
    std::ofstream output_file;
    if( options.is_set( "out" ) )
    {
        output_file.open( options[ "out" ].c_str( ) );
    }
    std::ostream &out = options.is_set( "out" ) ? output_file : std::cout;
    
    /* Create GLM */
    glm_model *model = NULL;
    if( options[ "model" ] == "binomial" )
    {
        std::string link = "logit";
        if( options.is_set( "link_function" ) )
        {
            link = options[ "link_function" ];
        }

        model= new binomial( link );
    }
    else if( options[ "model" ] == "normal" )
    {
        std::string link = "identity";
        if( options.is_set( "link_function" ) )
        {
            link = options[ "link_function" ];
        }

        model = new normal( link );
    }

    run_env( genotypes, phenotype, cov, E, env_names, missing, *model, out );
    
    if( model != NULL )
    {
        delete model; 
    }

    return 0;
}
