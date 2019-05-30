#include <iostream>
#include <sstream>
#include <cfloat>

#include <armadillo>

#include <cpp-argparse/OptionParser.h>
#include <besiq/io/covariates.hpp>
#include <besiq/stats/snp_count.hpp>

#include <plink/plink_file.hpp>
#include <dcdflib/libdcdf.hpp>

#include "gene_environment.hpp"

using namespace arma;
using namespace optparse;

const std::string USAGE = "besiq-lars [OPTIONS] plink_file";
const std::string DESCRIPTION = "Uses the LARS algorithm to find important loci.";
const std::string VERSION = "besiq 0.0.1";
const std::string EPILOG = "";

class lars_variables;
class lars_result;
arma::vec lars(lars_variables &variable_set, lars_result &result, size_t max_vars = 15, bool lasso = true, double threshold = 1.0);

/**
 * This class is responsible for keeping track which variables
 * are currently in the active and inactive sets, excluding those
 * that are currently in the ignored set.
 */
class active_set
{
public:
    /**
     * Constructor.
     *
     * @param size Total number of variables ([0...size-1]).
     */
    active_set(size_t size)
    {
        m_active = std::set<unsigned int>( );
        m_ignored = std::set<unsigned int>( );
        m_size = size;
        m_last_added = -1;
    }

    /**
     * Add a variable to the active set. It is assumed that this
     * variable is not ignored.
     *
     * @param x Index of variable to add.
     */
    void add(unsigned int x)
    {
        m_last_added = x;
        m_active.insert( x );
    }

    /**
     * Ignores a parameters, ignored parameters are neither
     * active or inactive.
     *
     * @param x Index of variable to ignore.
     */
    void ignore(unsigned int x)
    {
        m_active.erase( x );
        m_ignored.insert( x );
    }

    /**
     * Move a variable from active to inactive.
     *
     * @param Index of the variable to move.
     */
    void drop(unsigned int x)
    {
        m_active.erase( x );
    }

    /**
     * Returns a vector of indicies of the currently active variables
     * suitable for the arma package.
     *
     * @return A vector of indicies of active variables.
     */
    arma::uvec get_active() const
    {
        arma::uvec active = arma::zeros<arma::uvec>( m_active.size( ) );
        std::set<unsigned int>::const_iterator it;
        unsigned int index = 0;
        for(it = m_active.begin( ); it != m_active.end( ); ++it)
        {
            active[ index ] = *it;
            index++;      
        }

        return active;
    }

    /**
     * Returns a vector of indicies of the currently inactive variables
     * suitable for the arma package.
     *
     * @return A vector of indicies of inactive variables.
     */
    arma::uvec get_inactive() const
    {
        arma::uvec inactive = arma::zeros<arma::uvec>( m_size - m_active.size( ) - m_ignored.size( ) );
        unsigned int index = 0;
        for(int i = 0; i < m_size; i++)
        {
            if( m_active.count( i ) > 0 || m_ignored.count( i ) > 0 )
            {
                continue;
            }

            inactive[ index ] = i;
            index++;
        }

        return inactive;
    }

    /**
     * Returns the last added variable to the active set.
     *
     * @return The last added variable to the active set.
     */
    unsigned int get_last_added() const
    {
        return m_last_added;
    }

    /**
     * Returns the number of active variables.
     *
     * @return the number of active variables.
 
*/
    size_t size() const
    {
        return m_active.size( );
    }

private:
    /**
     * Total number of variables.
     */
    size_t m_size;

    /**
     * Set of active variables.
     */
    std::set<unsigned int> m_active;

    /**
     * Set of ignored variables.
     */
    std::set<unsigned int> m_ignored;

    /**
     * Last added index.
     */
    unsigned int m_last_added;
};

struct knot_info
{
    std::string variable;
    unsigned int variable_index;
    double lambda;
    arma::uvec active;
    arma::mat X_active;
    arma::vec beta_active;
    arma::mat X_h0;
};

struct lars_knot
{
    knot_info info;
    bool remove;
    double T;
    double pvalue;
    double explained_var;
    bool valid_p;
};

class lars_result
{
public:
    lars_result(const arma::vec &phenotype, bool calculate_p) :
        m_phenotype( phenotype ),
        m_calculate_p( calculate_p )
    {
        m_pheno_var = arma::accu( pow( phenotype, 2 ) ) / (phenotype.n_elem - 1);
    }

    void init(double lambda)
    {
        lars_knot start;
        start.info.variable = "NULL";
        start.info.variable_index = -1;
        start.info.lambda = lambda;
        start.remove = false;
        start.T = 0.0;
        start.pvalue = 1.0;
        start.explained_var = 0.0;
        start.valid_p = false;

        m_knots.push_back( start );
    }

    void add_knot(bool remove, knot_info &info, double model_var)
    {
        lars_knot new_knot;
        new_knot.info = info;
        new_knot.remove = remove;
        new_knot.explained_var = 1.0 - model_var / m_pheno_var;

        if( remove || !m_calculate_p )
        {
            new_knot.T = 0;
            new_knot.pvalue = 1.0;

            m_knots.push_back( new_knot );

            return;
        }
 
        /* Calculate p for new beta and add last component of the path */
        double cur_cor = dot( info.X_active * info.beta_active, m_phenotype );

        /* Compute h0 */
        double prev_cor = 0;
        if( info.X_active.n_cols > 1 )
        {
            null_lars null( info.X_h0, m_phenotype );
            lars_result null_result( m_phenotype, false );
            lars( null, null_result, info.X_h0.n_cols, true );

            std::vector<lars_knot> null_knot = null_result.get_knots( );
            
            unsigned int knot_index = -1;
            for(int i = 1; i < null_knot.size( ); i++)
            {
                if( info.lambda < null_knot[ i - 1 ].info.lambda && info.lambda > null_knot[ i ].info.lambda )
                {
                    knot_index = i;
                }
            }

            lars_knot &prev = null_knot[ knot_index - 1 ];
            lars_knot &next = null_knot[ knot_index ];

            arma::vec beta_prev = arma::zeros<arma::vec>( info.X_h0.n_cols );
            align_beta( prev.info.beta_active, prev.info.active, &beta_prev );

            arma::vec beta_next = arma::zeros<arma::vec>( info.X_h0.n_cols );
            align_beta( next.info.beta_active, next.info.active, &beta_next );

            double lambda_prev = prev.info.lambda;
            double lambda_next = next.info.lambda;

            arma::vec k = (beta_next - beta_prev)/(lambda_next - lambda_prev);
            arma::vec beta_h0 = beta_prev + (info.lambda - lambda_prev) * k;

            prev_cor = dot( info.X_h0 * beta_h0, m_phenotype );
        }

        double T = (cur_cor - prev_cor ) / model_var;
        double p = 1.0;
        if( T > 0.0 )
        {
            p = 1 - exp_cdf( T, 1.0 );
            new_knot.valid_p = true;
        }
        else
        {
            new_knot.valid_p = false;
        }

        new_knot.T = T;
        new_knot.pvalue = p;
            
        m_knots.push_back( new_knot );
    }

    std::vector<lars_knot> get_knots()
    {
        return m_knots;
    }

    void align_beta(arma::vec &beta, arma::uvec &active, arma::vec *aligned_beta)
    {
        for(int i = 0; i < active.n_elem; i++)
        {
            (*aligned_beta)[ active[ i ] ] = beta[ i ];
        }
    }

    void write_result(std::ostream &out, bool only_pvalues)
    {
        if( only_pvalues )
        {
            out << "step\tvariable\taction\tbeta\tT\tp\tlambda\tbeta_sum\texplained_var\n";
            for(int i = 1; i < m_knots.size( ); i++)
            {
                lars_knot &knot = m_knots[ i ];
                std::string action = "add";
                arma::vec this_beta = knot.info.beta_active.elem( find( knot.info.active == knot.info.variable_index ) );
                if( knot.remove )
                {
                    action = "remove";
                    this_beta = arma::zeros<arma::vec>( 1 );
                }

                out << i << "\t" <<
                    knot.info.variable << "\t" <<
                    action << "\t" <<
                    this_beta[ 0 ] << "\t" <<
                    knot.T << "\t" <<
                    knot.pvalue << "\t" <<
                    knot.info.lambda <<  "\t" <<
                    arma::sum( arma::abs( knot.info.beta_active ) ) <<  "\t" <<
                    knot.explained_var << "\n";
            }
        }
        else
        {
            int var = 0;
            std::map<unsigned int, unsigned int> new_pos;
            std::vector<std::string> name;
            for(int i = 1; i < m_knots.size( ); i++)
            {
                lars_knot &knot = m_knots[ i ];
                if( knot.remove )
                {
                    continue;
                }

                if( new_pos.count( knot.info.variable_index ) <= 0 )
                {
                    new_pos[ knot.info.variable_index ] = var;
                    name.push_back( knot.info.variable );
                    var++;
                }
            }

            out << "step\tvariable\tT\tp\tbeta_sum\texplained_var\tlambda";
            for(int i = 0; i < name.size( ); i++)
            {
                out <<  "\t" << name[ i ];
            }
            out << "\n";

            for(int i = 1; i < m_knots.size( ); i++)
            {
                lars_knot &knot = m_knots[ i ];
                arma::vec beta = arma::zeros<arma::vec>( new_pos.size( ) );

                for(int j = 0; j < knot.info.active.n_elem; j++)
                {
                    beta[ new_pos[ knot.info.active[ j ] ] ] = knot.info.beta_active[ j ];
                }
                
                out << i << "\t" << knot.info.variable << "\t" << knot.T << "\t" << knot.pvalue << "\t" << arma::sum( arma::abs( knot.info.beta_active ) ) << "\t" << knot.explained_var << "\t" << knot.info.lambda << "\t";
                for(int j = 0; j < beta.n_elem; j++)
                {
                    out << "\t" << beta[ j ];
                }
                out << "\n";
            }

        }
    }

private:
    arma::vec m_phenotype;
    bool m_calculate_p;
    double m_pheno_var;
    std::vector<lars_knot> m_knots;
};


arma::vec
nice_division(arma::vec &a, arma::vec &b)
{
    b.elem( find( b < DBL_MIN ) ).fill( DBL_MIN );
    arma::vec x = a / b;
    x.elem( find( x <= 0 ) ).fill( DBL_MAX );
    
    return x;
}

/**
 * Solves the lasso problem for a given lambda. Used for computing the
 * null model during significance testing. The algorithm is currently
 * a pathwise coordinate descent.
 *
 * @param X Covariates.
 * @param y Outcome.
 * @param lambda Shrinkage factor.
 * @param start Starting values for beta.
 * @max_num_iter Maximum number of iterations before giving up.
 */
arma::vec optimize_lars_gd(const arma::mat &X, const arma::mat &y, double lambda, const arma::vec &start, size_t max_num_iter = 50)
{
    arma::vec beta = start;
    arma::vec r = y - X * beta;
    double prev_rss = 0;
    double cur_rss = sum( r % r );
    int num_iter = 0;

    while( std::abs( prev_rss - cur_rss ) / prev_rss > 1e-20 && num_iter++ < max_num_iter )
    {
        prev_rss = cur_rss;
        for(int j = 0; j < X.n_cols; j++)
        {
            double beta_star = arma::dot( X.col( j ), r ) + beta[ j ];
            double update = std::abs( beta_star ) - lambda;
            update = (update > 0) ? update : 0;

            double new_beta = (beta_star > 0) ? update : -update;
            double beta_increase = new_beta - beta[ j ];

            beta[ j ] = new_beta;

            if( std::abs( beta_increase ) > 0 )
            {
                r = r - X.col( j ) * beta_increase;
            }
        }
        cur_rss = sum( r % r );
    }

    return beta;
}

/**
 * Finds the maximum correlation and returns its index.
 *
 * @param v A vector of correlations.
 * @param inactive A subset of indicies to consider.
 * @param max_index The index of the maximum value in v will be stored here.
 *
 * @reutrn The maximum value of v restricted to the indicies in inactive.
 */
double
find_max(const arma::vec &v, const arma::uvec &inactive, unsigned int *max_index)
{
    double max_value = 0;
    *max_index = v.n_elem;
    for(int i = 0; i < inactive.n_elem; i++)
    {
        unsigned int cur_index = inactive[ i ];
        double cur_value = v[ cur_index ];

        // Notice the order of these two statements
        *max_index = ( cur_value > max_value ) ? cur_index : *max_index;
        max_value = ( cur_value > max_value ) ? cur_value : max_value;
    }

    return max_value;
}

arma::vec
lars(lars_variables &variable_set, lars_result &result, size_t max_vars, bool lasso, double threshold)
{
    arma::vec phenotype = variable_set.get_centered_phenotype( );
    size_t n = variable_set.get_num_samples( );
    size_t m = variable_set.get_num_variables( );

    arma::vec mu = arma::zeros<arma::vec>( n );
    arma::vec beta = arma::zeros<arma::vec>( m );
    arma::vec c = arma::zeros<arma::vec>( m );

    active_set active( m );
    arma::uvec inactive;
    arma::uvec drop;
    arma::uvec prev_active;
    double eps = 1e-10;

    /* It is the first lambda that needs to be added not the last... */
    variable_set.calculate_cor( phenotype - mu, c );
    result.init( arma::max( abs( c ) ) );

    int i = 0;
    while( active.size( ) < std::min( max_vars, m ) )
    {
        variable_set.calculate_cor( phenotype - mu, c );
        arma::vec cabs = abs( c );
        unsigned int max_index;
        double C = find_max( cabs, active.get_inactive( ), &max_index );

        if( C < eps )
        {
            break;
        }
       
        /* If previous step was a drop we should not add
         * more variables in this step only increase beta. */
        bool prev_was_drop = drop.n_elem > 0;
        if( !prev_was_drop )
        {
            prev_active = active.get_active( ); 
            active.add( max_index );
        }
        inactive = active.get_inactive( );

        /* Create the active matrix */
        arma::mat X_active = variable_set.get_active( active.get_active( ) );

        /* Equation 2.4 */
        arma::vec s = sign( c.elem( active.get_active( ) ) );

        /* Equation 2.5 */
        arma::mat G = X_active.t( ) * X_active;
        arma::mat Ginv;
        if( !pinv( Ginv, G ) )
        {
            active.ignore( max_index );
            continue;
        }
        double A = 1.0 / sqrt( arma::accu( s.t( ) * Ginv * s ) );

        /* Equation 2.6 */
        arma::vec w = A * Ginv * s;
        arma::vec u = X_active * w;

        double gamma = C / A;
        if( active.size( ) < m )
        { 
            /* Equation 2.11 */
            arma::vec a = variable_set.eig_prod( u );

            /* Equation 2.13 */
            arma::vec cc = c.elem( inactive );
            arma::vec ac = a.elem( inactive );
            
            arma::vec a1 = C - cc;
            arma::vec b1 = A - ac;

            arma::vec gn = nice_division( a1, b1 );

            arma::vec a2 = C + cc;
            arma::vec b2 = A + ac;
            arma::vec gp = nice_division( a2, b2 );

            gamma = std::min( gn.min( ), gp.min( ) );
        }

        if( lasso )
        {
            drop.clear( );
            /* Equation 3.4 */
            arma::vec gammaj = -beta.elem( active.get_active( ) ) / w;

            /* Equation 3.5 */
            arma::uvec valid_elems = arma::find( gammaj > eps );
            if( valid_elems.n_elem > 0 )
            {
                double gammatilde = std::min( arma::min( gammaj.elem( valid_elems ) ), gamma );

                if( gammatilde < gamma )
                {
                    gamma = gammatilde;
                    drop = active.get_active( ).elem( find( gammaj == gammatilde ) );
                    assert( drop.n_elem == 1 );
                }
            }
        }

        beta.elem( active.get_active( ) ) = beta.elem( active.get_active( ) ) + w * gamma;
        mu = mu + gamma * u;

        bool cur_is_drop = drop.n_elem > 0;
        knot_info info;
        if( cur_is_drop )
        {
            /* This knot is a deletion, so zero out beta and remove the deleted
             * variable from the active set, and in the next step we need to
             * increase beta to get to the knot of the newly added variable. */
            beta.elem( drop ).fill( 0.0 );
            active.drop( drop[ 0 ] );
        }
        
        /**
         * If addition we use the max index, if deletion we use the
         * drop index, if previous was a drop we use the last added
         * variable because it is that one we are moving forward with.
         */
        if( !cur_is_drop && !prev_was_drop )
        {
            info.variable_index = max_index;
        }
        else if( cur_is_drop )
        {
            info.variable_index = drop[ 0 ];
        }
        else
        {
            info.variable_index = active.get_last_added( );
        }
        info.variable = variable_set.get_name( info.variable_index );

        info.active = active.get_active( );
        info.beta_active = beta.elem( info.active );
        info.X_active = !cur_is_drop ? X_active : variable_set.get_active( info.active );

        double model_var = sum( pow( phenotype - mu, 2 ) ) / (n - 1 - active.size( ) );
        arma::vec r = phenotype - mu;
        s = sign( c.elem( active.get_active( ) ) );
        info.lambda = arma::max( s % ( info.X_active.t( ) * r ) );

        if( prev_active.n_elem > 0 )
        {
            info.X_h0 = variable_set.get_active( prev_active );
        }

        result.add_knot( cur_is_drop, info, model_var );

        i++;
    }
    
    return beta;
}

int
main(int argc, char *argv[])
{
    OptionParser parser = OptionParser( ).usage( USAGE )
                                         .version( VERSION )
                                         .description( DESCRIPTION )
                                         .epilog( EPILOG ); 
 
    parser.add_option( "-p", "--pheno" ).help( "Read phenotypes from this file instead of a plink file." );
    parser.add_option( "-e", "--mpheno" ).help( "Name of the phenotype that you want to read (if there are more than one in the phenotype file)." );
    parser.add_option( "-c", "--cov" ).action( "store" ).type( "string" ).metavar( "filename" ).help( "Performs the analysis by including the covariates in this file." );
    parser.add_option( "-o", "--out" ).help( "The output file that will contain the results (binary)." );
    parser.add_option( "-m", "--max-variables" ).help( "Maximum number of variables in the model" ).set_default( 10 );
    parser.add_option( "-t", "--threshold" ).help( "Stop after a variable has a p-value less than this threshold." ).set_default( 1.0 );
    parser.add_option( "-a", "--maf" ).help( "Filter variants with maf (it is important to set this to avoid interactions with monotonic snps)" ).set_default( 0.05 );
    parser.add_option( "--only-pvalues" ).help( "Only output the beta that enters in each step along with its p-value." ).action( "store_true" );
    parser.add_option( "--only-main" ).help( "Only output the main effects." ).action( "store_true" );

    Values options = parser.parse_args( argc, argv );
    if( parser.args( ).size( ) != 1 )
    {
        parser.print_help( );
        exit( 1 );
    }

    std::ios_base::sync_with_stdio( false );
    
    /* Read all genotypes */
    plink_file_ptr genotype_file = open_plink_file( parser.args( )[ 0 ] );
    genotype_matrix_ptr genotypes = create_filtered_genotype_matrix( genotype_file, (float) options.get( "maf" ) );
    std::vector<std::string> order = genotype_file->get_sample_iids( );

    /* Parse phenotypes */
    arma::uvec cov_missing = arma::zeros<arma::uvec>( genotype_file->get_samples( ).size( ) );
    arma::uvec pheno_missing = arma::zeros<arma::uvec>( genotype_file->get_samples( ).size( ) );
    arma::vec phenotype;
    arma::mat cov;
    std::vector<std::string> cov_names;
    if( options.is_set( "pheno" ) )
    {
        std::ifstream phenotype_file( options[ "pheno" ].c_str( ) );
        phenotype = parse_phenotypes( phenotype_file, pheno_missing, order, options[ "mpheno" ] );
    }
    else
    {
        phenotype = create_phenotype_vector( genotype_file->get_samples( ), pheno_missing );
    }
    if( options.is_set( "cov" ) )
    {
        std::ifstream covariate_file( options[ "cov" ].c_str( ) );
        cov = parse_covariate_matrix( covariate_file, cov_missing, order, &cov_names );
    }

    /* Open output stream */
    std::ofstream output_file;
    if( options.is_set( "out" ) )
    {
        output_file.open( options[ "out" ].c_str( ) );
    }
    std::ostream &out = options.is_set( "out" ) ? output_file : std::cout;

    bool only_pvalues = options.is_set( "only_pvalues" );
    bool only_main = options.is_set( "only_main" );

    gene_environment variable_set( genotypes, cov, phenotype, cov_names, only_main );
    variable_set.impute_missing( );
    lars_result result( variable_set.get_centered_phenotype( ), true );
    lars( variable_set, result, (int) options.get( "max_variables" ), (double) options.get( "threshold" ) );
    result.write_result( out, only_pvalues );

    return 0;
}
