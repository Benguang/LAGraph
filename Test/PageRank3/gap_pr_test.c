//------------------------------------------------------------------------------
// p3test: read in (or create) a matrix and test PageRank3
//------------------------------------------------------------------------------

/*
LAGraph:  graph algorithms based on GraphBLAS

Copyright 2019 LAGraph Contributors.

(see Contributors.txt for a full list of Contributors; see
ContributionInstructions.txt for information on how you can Contribute to
this project).

All Rights Reserved.

NO WARRANTY. THIS MATERIAL IS FURNISHED ON AN "AS-IS" BASIS. THE LAGRAPH
CONTRIBUTORS MAKE NO WARRANTIES OF ANY KIND, EITHER EXPRESSED OR IMPLIED,
AS TO ANY MATTER INCLUDING, BUT NOT LIMITED TO, WARRANTY OF FITNESS FOR
PURPOSE OR MERCHANTABILITY, EXCLUSIVITY, OR RESULTS OBTAINED FROM USE OF
THE MATERIAL. THE CONTRIBUTORS DO NOT MAKE ANY WARRANTY OF ANY KIND WITH
RESPECT TO FREEDOM FROM PATENT, TRADEMARK, OR COPYRIGHT INFRINGEMENT.

Released under a BSD license, please see the LICENSE file distributed with
this Software or contact permission@sei.cmu.edu for full terms.

Created, in part, with funding and support from the United States
Government.  (see Acknowledgments.txt file).

This program includes and/or can make use of certain third party source
code, object code, documentation and other files ("Third Party Software").
See LICENSE file for more details.

*/

//------------------------------------------------------------------------------

// Contributed by Tim Davis, Texas A&M and Gabor Szarnyas, BME

// usage:
// p3test < in > out

#define NTHREAD_LIST 6
#define THREAD_LIST 64, 32, 24, 12, 8, 4

#include "LAGraph.h"

#define LAGRAPH_FREE_ALL                        \
{                                               \
    if (P != NULL) { free (P) ; P = NULL ; }    \
    GrB_free (&A) ;                             \
    GrB_free (&A2) ;                            \
    GrB_free (&PR) ;                            \
    GrB_free (&d_in) ;                          \
    GrB_free (&d_out) ;                         \
    GrB_free (&A_temp) ;                        \
}

int main (int argc, char **argv)
{

    GrB_Info info ;
    GrB_Matrix A = NULL ;
    GrB_Matrix A_temp = NULL ;
    GrB_Matrix A2 = NULL ;
    LAGraph_PageRank *P = NULL ;
    GrB_Vector PR = NULL;
    GrB_Vector d_out = NULL, d_in = NULL ;

    LAGraph_init ( ) ;

    int nt = NTHREAD_LIST ;
    int Nthreads [20] = { 0, THREAD_LIST } ;
    int nthreads_max = LAGraph_get_nthreads();
    Nthreads [nt] = LAGRAPH_MIN (Nthreads [nt], nthreads_max) ;
    for (int t = 1 ; t <= nt ; t++)
    {
        int nthreads = Nthreads [t] ;
        if (nthreads > nthreads_max) continue ;
        printf (" thread test %d: %d\n", t, nthreads) ;
    }

    //--------------------------------------------------------------------------
    // read in a matrix from a file and convert to boolean
    //--------------------------------------------------------------------------

    double tic [2] ;
    LAGraph_tic (tic) ;

    if (argc > 1)
    {
        // Usage:
        //      ./p3test matrixfile.mtx
        //      ./p3test matrixfile.grb

        // read in the file in Matrix Market format from the input file
        char *filename = argv [1] ;
        printf ("matrix: %s\n", filename) ;

        // find the filename extension
        size_t len = strlen (filename) ;
        char *ext = NULL ;
        for (int k = len-1 ; k >= 0 ; k--)
        {
            if (filename [k] == '.')
            {
                ext = filename + k ;
                printf ("[%s]\n", ext) ;
                break ;
            }
        }
        bool is_binary = (ext != NULL && strncmp (ext, ".grb", 4) == 0) ;

        if (is_binary)
        {
            printf ("Reading binary file: %s\n", filename) ;
            LAGRAPH_OK (LAGraph_binread (&A, filename)) ;
        }
        else
        {
            printf ("Reading Matrix Market file: %s\n", filename) ;
            FILE *f = fopen (filename, "r") ;
            if (f == NULL)
            {
                printf ("Matrix file not found: [%s]\n", filename) ;
                exit (1) ;
            }
            LAGRAPH_OK (LAGraph_mmread(&A, f));
            fclose (f) ;
        }

    }
    else
    {

        // Usage:  ./p3test < matrixfile.mtx
        printf ("matrix: from stdin\n") ;

        // read in the file in Matrix Market format from stdin
        LAGRAPH_OK (LAGraph_mmread(&A, stdin));
    }


    // GxB_fprint (A, GxB_SHORT, stdout) ;
    // LAGraph_mmwrite (A, stdout) ;

    // convert to bool
    LAGRAPH_OK (LAGraph_pattern (&A_temp, A, GrB_BOOL)) ;
    // LAGraph_mmwrite (A_temp, stdout) ;
    GrB_free (&A) ;
    A = A_temp ;
    A_temp = NULL ;
    LAGRAPH_OK(GxB_set (A, GxB_FORMAT, GxB_BY_COL));
    // GxB_fprint (A, GxB_COMPLETE, stdout) ;

    //--------------------------------------------------------------------------
    // get the size of the problem.
    //--------------------------------------------------------------------------

    GrB_Index nrows, ncols ;
    LAGRAPH_OK (GrB_Matrix_nrows (&nrows, A)) ;
    LAGRAPH_OK (GrB_Matrix_ncols (&ncols, A)) ;
    GrB_Index n = nrows ;

    // finish any pending computations
    GrB_Index nvals ;
    GrB_Matrix_nvals (&nvals, A) ;
    GrB_Matrix_dup (&A2, A) ;
    printf ("original # of edges: %"PRId64"\n", nvals) ;

    //--------------------------------------------------------------------------
    // ensure the matrix has no empty rows or columns
    //--------------------------------------------------------------------------

    LAGRAPH_OK (GrB_Vector_new (&d_out, GrB_FP32, n)) ;
    LAGRAPH_OK (GrB_Vector_new (&d_in , GrB_FP32, n)) ;
    LAGRAPH_OK (GrB_reduce (d_out, NULL, NULL, GxB_PLUS_FP32_MONOID, A, NULL ));
    LAGRAPH_OK (GrB_reduce (d_in,  NULL, NULL, GxB_PLUS_FP32_MONOID, A,
        LAGraph_desc_tooo)) ;
    GrB_Index n_d_out, n_d_in ;
    LAGRAPH_OK (GrB_Vector_nvals (&n_d_out, d_out)) ;
    LAGRAPH_OK (GrB_Vector_nvals (&n_d_in , d_in )) ;

    int64_t edges_added = 0 ;
    if (n_d_out < n || n_d_in < n)
    {
        // A = A+I if A has any dangling nodes
        printf ("Matrix has %"PRId64" empty rows\n", n - n_d_out) ;
        printf ("Matrix has %"PRId64" empty cols\n", n - n_d_in) ;
        for (GrB_Index i = 0; i < n; i++)
        {
            float din = 0, dout = 0 ;
            LAGRAPH_OK (GrB_Vector_extractElement (&din , d_in , i)) ;
            LAGRAPH_OK (GrB_Vector_extractElement (&dout, d_out, i)) ;
            if (din == 0 || dout == 0)
            {
                edges_added++ ;
                LAGRAPH_OK (GrB_Matrix_setElement (A, 1, i, i));
            }
        }
    }

    GrB_free (&d_in) ;
    GrB_free (&d_out) ;

    // Matrix A row sum
    // Stores the outbound degrees of all vertices, for pagerank3e and 3d
    LAGr_Vector_new (&d_out, GrB_FP32, n) ;
    LAGr_reduce (d_out, NULL, NULL, GxB_PLUS_FP32_MONOID, A, NULL) ;
    GrB_Index non_dangling ;
    LAGr_Vector_nvals (&non_dangling, d_out) ;
    if (non_dangling < n)
    {
        LAGRAPH_ERROR ("Matrix has dangling nodes!", GrB_INVALID_VALUE) ;
    }

    // copy into a double vector for pagerank3c
    float *dout = NULL ;
    GrB_Index *dI = NULL ;
    GrB_Type dtype ;
    GrB_Vector d_out2 = NULL ;
    GrB_Vector_dup (&d_out2, d_out) ;
    LAGr_Vector_export (&d_out2, &dtype, &n, &n_d_out, &dI,
        (void **) (&dout), NULL) ;
    LAGRAPH_FREE (dI) ;

    printf ("\n=========="
            "input graph: nodes: %"PRIu64" edges: %"PRIu64"\n", n, nvals) ;
    printf ("diag entries added: %"PRId64"\n", edges_added) ;

    GrB_Index nvals2 ;
    GrB_Matrix_nvals (&nvals2, A) ;

    double tread = LAGraph_toc (tic) ;
    printf ("read time: %g sec\n", tread) ;

    FILE *f = NULL ;

    // GxB_fprint (A, GxB_SHORT, stdout) ;

    //--------------------------------------------------------------------------
    // compute the pagerank (both methods)
    //--------------------------------------------------------------------------

    // the GAP benchmark requires 16 trials
    int ntrials = 16 ;
    printf ("# of trials: %d\n", ntrials) ;

    float tol = 1e-4 ;
    int iters, itermax = 100 ;

    double chunk ; // = 64 * 1024 ;
    GxB_get (GxB_CHUNK, &chunk) ;
    printf ("chunk: %g\n", chunk) ;

    //--------------------------------------------------------------------------
    // method 3e
    //--------------------------------------------------------------------------

#if 0
    printf ("\nMethod 3e:\n") ;
    for (int kk = 1 ; kk <= nt ; kk++)
    {
        int nthreads = Nthreads [kk] ;
        if (nthreads > nthreads_max) continue ;
        LAGraph_set_nthreads (nthreads) ;
        // printf ("3e:%2d: ", nthreads) ;
        
        double total_time = 0 ;

        for (int trial = 0 ; trial < ntrials ; trial++)
        {
            GrB_free (&PR) ;
            LAGraph_tic (tic) ;
            LAGRAPH_OK (LAGraph_pagerank3e (&PR, A, d_out, 0.85, itermax,
                &iters)) ;
            double t1 = LAGraph_toc (tic) ;
            // printf ("%10.3f ", t1) ;
            total_time += t1 ;
        }
        // printf ("\n") ;

        double t = total_time / ntrials ;

        printf ("3e:%3d: avg time: %10.3f (sec), "
                "rate: %10.3f iters: %d\n", nthreads,
                t, 1e-6*((double) nvals) * iters / t, iters) ;

    }

    // GxB_print (PR, GxB_SHORT) ;

    // f = fopen ("rank3e.mtx", "w") ;
    // LAGraph_mmwrite (PR, f) ;
    // fclose (f) ;

    GrB_free (&PR) ;
#endif

    //--------------------------------------------------------------------------
    // method 3d
    //--------------------------------------------------------------------------

#if 0
    printf ("\nMethod 3d:\n") ;
    for (int kk = 1 ; kk <= nt ; kk++)
    {
        int nthreads = Nthreads [kk] ;
        if (nthreads > nthreads_max) continue ;
        LAGraph_set_nthreads (nthreads) ;
        // printf ("3d:%2d: ", nthreads) ;
        
        double total_time = 0 ;

        for (int trial = 0 ; trial < ntrials ; trial++)
        {
            GrB_free (&PR) ;
            LAGraph_tic (tic) ;
            LAGRAPH_OK (LAGraph_pagerank3d (&PR, A, d_out, 0.85, itermax,
                &iters)) ;
            double t1 = LAGraph_toc (tic) ;
            // printf ("%10.3f ", t1) ; fflush (stdout) ;
            total_time += t1 ;
        }
        // printf ("\n") ;

        double t = total_time / ntrials ;
        printf ("3d:%3d: avg time: %10.3f (sec), "
                "rate: %10.3f iters: %d\n", nthreads,
                t, 1e-6*((double) nvals) * iters / t, iters) ;
    }

    // GxB_print (PR, GxB_SHORT) ;

    // f = fopen ("rank3d.mtx", "w") ;
    // LAGraph_mmwrite (PR, f) ;
    // fclose (f) ;

    GrB_free (&PR) ;
#endif

    //--------------------------------------------------------------------------
    // method 3c
    //--------------------------------------------------------------------------

    printf ("\nMethod 3c:\n") ;
    for (int kk = 1 ; kk <= nt ; kk++)
    {
        int nthreads = Nthreads [kk] ;
        if (nthreads > nthreads_max) continue ;
        LAGraph_set_nthreads (nthreads) ;
        // printf ("3c:%2d: ", nthreads) ;

        double total_time = 0 ;

        for (int trial = 0 ; trial < ntrials ; trial++)
        {
            GrB_free (&PR) ;
            LAGraph_tic (tic) ;
            LAGRAPH_OK (LAGraph_pagerank3c (&PR, A, dout, 0.85, itermax,
                &iters)) ;
            double t1 = LAGraph_toc (tic) ;
            // printf ("%10.3f ", t1) ; fflush (stdout) ;
            total_time += t1 ;
        }
        // printf ("\n") ;

        double t = total_time / ntrials ;
        printf ("3c:%3d: avg time: %10.3f (sec), "
                "rate: %10.3f iters: %d\n", nthreads,
                t, 1e-6*((double) nvals) * iters / t, iters) ;
    }

    // f = fopen ("rank3c.mtx", "w") ;
    // LAGraph_mmwrite (PR, f) ;
    // fclose (f) ;

    // GxB_print (PR, GxB_SHORT) ;

    //--------------------------------------------------------------------------
    // method 3f
    //--------------------------------------------------------------------------

    printf ("\nMethod 3f:\n") ;
    for (int kk = 1 ; kk <= nt ; kk++)
    {
        int nthreads = Nthreads [kk] ;
        if (nthreads > nthreads_max) continue ;
        LAGraph_set_nthreads (nthreads) ;
        // printf ("3c:%2d: ", nthreads) ;

        double total_time = 0 ;

        for (int trial = 0 ; trial < ntrials ; trial++)
        {
            GrB_free (&PR) ;
            LAGraph_tic (tic) ;
            LAGRAPH_OK (LAGraph_pagerank3f (&PR, A2, d_out, 0.85, itermax,
                &iters)) ;
            double t1 = LAGraph_toc (tic) ;
            // printf ("%10.3f ", t1) ; fflush (stdout) ;
            total_time += t1 ;
        }
        // printf ("\n") ;

        double t = total_time / ntrials ;
        printf ("3f:%3d: avg time: %10.3f (sec), "
                "rate: %10.3f iters: %d\n", nthreads,
                t, 1e-6*((double) nvals) * iters / t, iters) ;
    }

    // f = fopen ("rank3f.mtx", "w") ;
    // LAGraph_mmwrite (PR, f) ;
    // fclose (f) ;

    // GxB_print (PR, GxB_SHORT) ;

    //--------------------------------------------------------------------------
    // free all workspace and finish
    //--------------------------------------------------------------------------

    LAGRAPH_FREE_ALL ;
    LAGRAPH_OK (LAGraph_finalize ( )) ;
    return (GrB_SUCCESS) ;
}

