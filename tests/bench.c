/**************************************************************************/
/* NOTE to users: this is the FFTW self-test and benchmark program.
   It is probably NOT a good place to learn FFTW usage, since it has a
   lot of added complexity in order to exercise and test the full API,
   etcetera.  We suggest reading the manual. */
/**************************************************************************/

#include "bench-user.h"
#include <math.h>
#include <stdio.h>
#include <fftw3.h>
#include <string.h>

BEGIN_BENCH_DOC
BENCH_DOC("name", "fftw3")
END_BENCH_DOC 

#define CONCAT(prefix, name) prefix ## name
#if defined(BENCHFFT_SINGLE)
#define FFTW(x) CONCAT(fftwf_, x)
#elif defined(BENCHFFT_LDOUBLE)
#define FFTW(x) CONCAT(fftwl_, x)
#else
#define FFTW(x) CONCAT(fftw_, x)
#endif

FFTW(plan) the_plan = 0;

static const char *wisdat = "wis.dat";
unsigned the_flags = 0;
int paranoid = 0;
int usewisdom = 1;
int havewisdom = 0;
int nthreads = 1;

extern void install_hook(void);  /* in hook.c */
extern void uninstall_hook(void);  /* in hook.c */

void useropt(const char *arg)
{
     if (!strcmp(arg, "patient")) the_flags |= FFTW_PATIENT;
     else if (!strcmp(arg, "estimate")) the_flags |= FFTW_ESTIMATE;
     else if (!strcmp(arg, "exhaustive")) the_flags |= FFTW_EXHAUSTIVE;
     else if (!strcmp(arg, "paranoid")) paranoid = 1;
     else if (!strcmp(arg, "nowisdom")) usewisdom = 0;

     else fprintf(stderr, "unknown user option: %s.  Ignoring.\n", arg);
}

void rdwisdom(void)
{
     FILE *f;
     double tim;

     if (!usewisdom) return;
     if (havewisdom) return;

     timer_start();
     if ((f = fopen(wisdat, "r"))) {
	  if (!FFTW(import_wisdom_from_file)(f))
	       fprintf(stderr, "bench: ERROR reading wisdom\n");
	  fclose(f);
     }
     tim = timer_stop();

     if (verbose > 1) printf("READ WISDOM (%g seconds): ", tim);

     if (verbose > 3)
	  FFTW(export_wisdom_to_file)(stdout);
     if (verbose > 1)
	  printf("\n");
     havewisdom = 1;
}

void wrwisdom(void)
{
     FILE *f;
     double tim;
     if (!usewisdom) return;

     timer_start();
     if ((f = fopen(wisdat, "w"))) {
	  FFTW(export_wisdom_to_file)(f);
	  fclose(f);
     }
     tim = timer_stop();
     if (verbose > 1) printf("write wisdom took %g seconds\n", tim);
}

static FFTW(iodim) *bench_tensor_to_fftw_iodim(
     bench_tensor *t, int istride_factor, int ostride_factor)
{
     FFTW(iodim) *d;
     int i;

     BENCH_ASSERT(t->rnk >= 0);
     if (t->rnk == 0) return 0;
     
     d = (FFTW(iodim) *)bench_malloc(sizeof(FFTW(iodim)) * t->rnk);
     for (i = 0; i < t->rnk; ++i) {
	  d[i].n = t->dims[i].n;
	  d[i].is = t->dims[i].is * istride_factor;
	  d[i].os = t->dims[i].os * ostride_factor;
     }

     return d;
}

static void extract_reim(int sign, bench_complex *c, 
			 bench_real **r, bench_real **i)
{
     if (sign == FFTW_FORWARD) {
          *r = c[0] + 0;
          *i = c[0] + 1;
     } else {
          *r = c[0] + 1;
          *i = c[0] + 0;
     }
}

static void extract_reim_split(int sign, int size, bench_real *p,
			       bench_real **r, bench_real **i)
{
     if (sign == FFTW_FORWARD) {
          *r = p + 0;
          *i = p + size;
     } else {
          *r = p + size;
          *i = p + 0;
     }
}

static int sizeof_problem(bench_problem *p)
{
     return tensor_sz(p->sz) * tensor_sz(p->vecsz);
}

/* ouch */
static int expressible_as_api_many(bench_tensor *t)
{
     int i;

     BENCH_ASSERT(FINITE_RNK(t->rnk));

     i = t->rnk - 1;
     while (--i >= 0) {
	  bench_iodim *d = t->dims + i;
	  if (d[0].is % d[1].is) return 0;
	  if (d[0].os % d[1].os) return 0;
     }
     return 1;
}

static int *mkn(bench_tensor *t)
{
     int *n = bench_malloc(sizeof(int *) * t->rnk);
     int i;
     for (i = 0; i < t->rnk; ++i) 
	  n[i] = t->dims[i].n;
     return n;
}

static void mknembed_many(bench_tensor *t, int **inembedp, int **onembedp)
{
     int i;
     bench_iodim *d;
     int *inembed = bench_malloc(sizeof(int *) * t->rnk);
     int *onembed = bench_malloc(sizeof(int *) * t->rnk);

     BENCH_ASSERT(FINITE_RNK(t->rnk));
     *inembedp = inembed; *onembedp = onembed;

     i = t->rnk - 1;
     while (--i >= 0) {
	  d = t->dims + i;
	  inembed[i+1] = d[0].is / d[1].is;
	  onembed[i+1] = d[0].os / d[1].os;
     }
}

/* try to use the most appropriate API function.  Big mess. */

static int imax(int a, int b) { return (a > b ? a : b); }

static int halfish_sizeof_problem(bench_problem *p)
{
     int n2 = sizeof_problem(p);
     if (FINITE_RNK(p->sz->rnk) && p->sz->rnk > 0)
          n2 = (n2 / imax(p->sz->dims[p->sz->rnk - 1].n, 1)) *
               (p->sz->dims[p->sz->rnk - 1].n / 2 + 1);
     return n2;
}

static FFTW(plan) mkplan_real_split(bench_problem *p, int flags)
{
     FFTW(plan) pln;
     bench_tensor *sz = p->sz, *vecsz = p->vecsz;
     FFTW(iodim) *dims, *howmany_dims;
     bench_real *ri, *ii, *ro, *io;
     int n2 = halfish_sizeof_problem(p);

     extract_reim_split(FFTW_FORWARD, n2, p->in, &ri, &ii);
     extract_reim_split(FFTW_FORWARD, n2, p->out, &ro, &io);

     dims = bench_tensor_to_fftw_iodim(sz, 1, 1);
     howmany_dims = bench_tensor_to_fftw_iodim(vecsz, 1, 1);
     if (p->sign < 0) {
	  if (verbose > 2) printf("using plan_guru_dft_r2c\n");
	  pln = FFTW(plan_guru_dft_r2c)(sz->rnk, dims,
					vecsz->rnk, howmany_dims,
					ri, ro, io, flags);
     }
     else {
	  if (verbose > 2) printf("using plan_guru_dft_c2r\n");
	  pln = FFTW(plan_guru_dft_c2r)(sz->rnk, dims,
					vecsz->rnk, howmany_dims,
					ri, ii, ro, flags);
     }
     bench_free(dims);
     bench_free(howmany_dims);
     return pln;
}

static FFTW(plan) mkplan_real_interleaved(bench_problem *p, int flags)
{
     FFTW(plan) pln;
     bench_tensor *sz = p->sz, *vecsz = p->vecsz;

     if (vecsz->rnk == 0 && tensor_unitstridep(sz) 
	 && tensor_real_rowmajorp(sz, p->sign, p->in_place)) 
	  goto api_simple;
     
     if (vecsz->rnk == 1 && expressible_as_api_many(sz))
	  goto api_many;

     goto api_guru;

 api_simple:
     switch (sz->rnk) {
	 case 1:
	      if (p->sign < 0) {
		   if (verbose > 2) printf("using plan_dft_r2c_1d\n");
		   return FFTW(plan_dft_r2c_1d)(sz->dims[0].n, 
						p->in, p->out, flags);
	      }
	      else {
		   if (verbose > 2) printf("using plan_dft_c2r_1d\n");
		   return FFTW(plan_dft_c2r_1d)(sz->dims[0].n, 
						p->in, p->out, flags);
	      }
	      break;
	 case 2:
	      if (p->sign < 0) {
		   if (verbose > 2) printf("using plan_dft_r2c_2d\n");
		   return FFTW(plan_dft_r2c_2d)(sz->dims[0].n, sz->dims[1].n,
						p->in, p->out, flags);
	      }
	      else {
		   if (verbose > 2) printf("using plan_dft_c2r_2d\n");
		   return FFTW(plan_dft_c2r_2d)(sz->dims[0].n, sz->dims[1].n,
						p->in, p->out, flags);
	      }
	      break;
	 case 3:
	      if (p->sign < 0) {
		   if (verbose > 2) printf("using plan_dft_r2c_3d\n");
		   return FFTW(plan_dft_r2c_3d)(
			sz->dims[0].n, sz->dims[1].n, sz->dims[2].n,
			p->in, p->out, flags);
	      }
	      else {
		   if (verbose > 2) printf("using plan_dft_c2r_3d\n");
		   return FFTW(plan_dft_c2r_3d)(
			sz->dims[0].n, sz->dims[1].n, sz->dims[2].n,
			p->in, p->out, flags);
	      }
	      break;
	 default: {
	      int *n = mkn(sz);
	      if (p->sign < 0) {
		   if (verbose > 2) printf("using plan_dft_r2c\n");
		   pln = FFTW(plan_dft_r2c)(sz->rnk, n, p->in, p->out, flags);
	      }
	      else {
		   if (verbose > 2) printf("using plan_dft_c2r\n");
		   pln = FFTW(plan_dft_c2r)(sz->rnk, n, p->in, p->out, flags);
	      }
	      bench_free(n);
	      return pln;
	 }
     }

 api_many:
     {
	  int *n, *inembed, *onembed;
	  BENCH_ASSERT(vecsz->rnk == 1);
	  n = mkn(sz);
	  mknembed_many(sz, &inembed, &onembed);
	  if (p->sign < 0) {
	       if (verbose > 2) printf("using plan_many_dft_r2c\n");
	       pln = FFTW(plan_many_dft_r2c)(
		    sz->rnk, n, vecsz->dims[0].n, 
		    p->in, inembed,
		    sz->dims[sz->rnk - 1].is, vecsz->dims[0].is,
		    p->out, onembed,
		    sz->dims[sz->rnk - 1].os, vecsz->dims[0].os,
		    flags);
	  }
	  else {
	       if (verbose > 2) printf("using plan_many_dft_c2r\n");
	       pln = FFTW(plan_many_dft_c2r)(
		    sz->rnk, n, vecsz->dims[0].n, 
		    p->in, inembed,
		    sz->dims[sz->rnk - 1].is, vecsz->dims[0].is,
		    p->out, onembed,
		    sz->dims[sz->rnk - 1].os, vecsz->dims[0].os,
		    flags);
	  }
	  bench_free(n); bench_free(inembed); bench_free(onembed);
	  return pln;
     }

 api_guru:
     {
	  FFTW(iodim) *dims, *howmany_dims;
	  bench_real *ri, *ii, *ro, *io;

	  extract_reim(p->sign, p->in, &ri, &ii);
	  extract_reim(p->sign, p->out, &ro, &io);

	  if (p->sign < 0) {
	       dims = bench_tensor_to_fftw_iodim(sz, 1, 2);
	       howmany_dims = bench_tensor_to_fftw_iodim(vecsz, 1, 2);
	       if (verbose > 2) printf("using plan_guru_dft_r2c\n");
	       pln = FFTW(plan_guru_dft_r2c)(sz->rnk, dims,
					     vecsz->rnk, howmany_dims,
					     ri, ro, io, flags);
	  }
	  else {
	       dims = bench_tensor_to_fftw_iodim(sz, 2, 1);
	       howmany_dims = bench_tensor_to_fftw_iodim(vecsz, 2, 1);
	       if (verbose > 2) printf("using plan_guru_dft_c2r\n");
	       pln = FFTW(plan_guru_dft_c2r)(sz->rnk, dims,
					     vecsz->rnk, howmany_dims,
					     ri, ii, ro, flags);
	  }
	  bench_free(dims);
	  bench_free(howmany_dims);
	  return pln;
     }
}

static FFTW(plan) mkplan_real(bench_problem *p, int flags)
{
     if (p->split)
	  return mkplan_real_split(p, flags);
     else
	  return mkplan_real_interleaved(p, flags);
}

static FFTW(plan) mkplan_complex_split(bench_problem *p, int flags)
{
     FFTW(plan) pln;
     bench_tensor *sz = p->sz, *vecsz = p->vecsz;
     FFTW(iodim) *dims, *howmany_dims;
     bench_real *ri, *ii, *ro, *io;
     int n = sizeof_problem(p);

     extract_reim_split(p->sign, n, p->in, &ri, &ii);
     extract_reim_split(p->sign, n, p->out, &ro, &io);

     dims = bench_tensor_to_fftw_iodim(sz, 1, 1);
     howmany_dims = bench_tensor_to_fftw_iodim(vecsz, 1, 1);
     if (verbose > 2) printf("using plan_guru_dft\n");
     pln = FFTW(plan_guru_dft)(sz->rnk, dims,
			       vecsz->rnk, howmany_dims,
			       ri, ii, ro, io, flags);
     bench_free(dims);
     bench_free(howmany_dims);
     return pln;
}

static FFTW(plan) mkplan_complex_interleaved(bench_problem *p, int flags)
{
     FFTW(plan) pln;
     bench_tensor *sz = p->sz, *vecsz = p->vecsz;

     if (vecsz->rnk == 0 && tensor_unitstridep(sz) && tensor_rowmajorp(sz)) 
	  goto api_simple;
     
     if (vecsz->rnk == 1 && expressible_as_api_many(sz))
	  goto api_many;

     goto api_guru;

 api_simple:
     switch (sz->rnk) {
	 case 1:
	      if (verbose > 2) printf("using plan_dft_1d\n");
	      return FFTW(plan_dft_1d)(sz->dims[0].n, 
				       p->in, p->out, 
				       p->sign, flags);
	      break;
	 case 2:
	      if (verbose > 2) printf("using plan_dft_2d\n");
	      return FFTW(plan_dft_2d)(sz->dims[0].n, sz->dims[1].n,
				       p->in, p->out, 
				       p->sign, flags);
	      break;
	 case 3:
	      if (verbose > 2) printf("using plan_dft_3d\n");
	      return FFTW(plan_dft_3d)(
		   sz->dims[0].n, sz->dims[1].n, sz->dims[2].n,
		   p->in, p->out, 
		   p->sign, flags);
	      break;
	 default: {
	      int *n = mkn(sz);
	      if (verbose > 2) printf("using plan_dft\n");
	      pln = FFTW(plan_dft)(sz->rnk, n, p->in, p->out, p->sign, flags);
	      bench_free(n);
	      return pln;
	 }
     }

 api_many:
     {
	  int *n, *inembed, *onembed;
	  BENCH_ASSERT(vecsz->rnk == 1);
	  n = mkn(sz);
	  mknembed_many(sz, &inembed, &onembed);
	  if (verbose > 2) printf("using plan_many_dft\n");
	  pln = FFTW(plan_many_dft)(
	       sz->rnk, n, vecsz->dims[0].n, 
	       p->in, inembed, sz->dims[sz->rnk - 1].is, vecsz->dims[0].is,
	       p->out, onembed, sz->dims[sz->rnk - 1].os, vecsz->dims[0].os,
	       p->sign, flags);
	  bench_free(n); bench_free(inembed); bench_free(onembed);
	  return pln;
     }

 api_guru:
     {
	  FFTW(iodim) *dims, *howmany_dims;
	  bench_real *ri, *ii, *ro, *io;

	  extract_reim(p->sign, p->in, &ri, &ii);
	  extract_reim(p->sign, p->out, &ro, &io);

	  dims = bench_tensor_to_fftw_iodim(sz, 2, 2);
	  howmany_dims = bench_tensor_to_fftw_iodim(vecsz, 2, 2);
	  if (verbose > 2) printf("using plan_guru_dft\n");
	  pln = FFTW(plan_guru_dft)(sz->rnk, dims,
				    vecsz->rnk, howmany_dims,
				    ri, ii, ro, io, flags);
	  bench_free(dims);
	  bench_free(howmany_dims);
	  return pln;
     }
}

static FFTW(plan) mkplan_complex(bench_problem *p, int flags)
{
     if (p->split)
	  return mkplan_complex_split(p, flags);
     else
	  return mkplan_complex_interleaved(p, flags);
}

static FFTW(plan) mkplan_r2r(bench_problem *p, int flags)
{
     FFTW(plan) pln;
     bench_tensor *sz = p->sz, *vecsz = p->vecsz;
     FFTW(r2r_kind) *k;

     k = bench_malloc(sizeof(FFTW(r2r_kind)) * sz->rnk);
     {
	  int i;
	  for (i = 0; i < sz->rnk; ++i)
	       switch (p->k[i]) {
		   case R2R_R2HC: k[i] = FFTW_R2HC; break;
		   case R2R_HC2R: k[i] = FFTW_HC2R; break;
		   case R2R_DHT: k[i] = FFTW_DHT; break;
		   case R2R_REDFT00: k[i] = FFTW_REDFT00; break;
		   case R2R_REDFT01: k[i] = FFTW_REDFT01; break;
		   case R2R_REDFT10: k[i] = FFTW_REDFT10; break;
		   case R2R_REDFT11: k[i] = FFTW_REDFT11; break;
		   case R2R_RODFT00: k[i] = FFTW_RODFT00; break;
		   case R2R_RODFT01: k[i] = FFTW_RODFT01; break;
		   case R2R_RODFT10: k[i] = FFTW_RODFT10; break;
		   case R2R_RODFT11: k[i] = FFTW_RODFT11; break;
		   default: BENCH_ASSERT(0);
	       }
     }

     if (vecsz->rnk == 0 && tensor_unitstridep(sz) && tensor_rowmajorp(sz)) 
	  goto api_simple;
     
     if (vecsz->rnk == 1 && expressible_as_api_many(sz))
	  goto api_many;

     goto api_guru;

 api_simple:
     switch (sz->rnk) {
	 case 1:
	      if (verbose > 2) printf("using plan_r2r_1d\n");
	      pln = FFTW(plan_r2r_1d)(sz->dims[0].n, 
				      p->in, p->out, 
				      k[0], flags);
	      goto done;
	 case 2:
	      if (verbose > 2) printf("using plan_r2r_2d\n");
	      pln = FFTW(plan_r2r_2d)(sz->dims[0].n, sz->dims[1].n,
				      p->in, p->out, 
				      k[0], k[1], flags);
	      goto done;
	 case 3:
	      if (verbose > 2) printf("using plan_r2r_3d\n");
	      pln = FFTW(plan_r2r_3d)(
		   sz->dims[0].n, sz->dims[1].n, sz->dims[2].n,
		   p->in, p->out, 
		   k[0], k[1], k[2], flags);
	      goto done;
	 default: {
	      int *n = mkn(sz);
	      if (verbose > 2) printf("using plan_r2r\n");
	      pln = FFTW(plan_r2r)(sz->rnk, n, p->in, p->out, k, flags);
	      bench_free(n);
	      goto done;
	 }
     }

 api_many:
     {
	  int *n, *inembed, *onembed;
	  BENCH_ASSERT(vecsz->rnk == 1);
	  n = mkn(sz);
	  mknembed_many(sz, &inembed, &onembed);
	  if (verbose > 2) printf("using plan_many_r2r\n");
	  pln = FFTW(plan_many_r2r)(
	       sz->rnk, n, vecsz->dims[0].n, 
	       p->in, inembed, sz->dims[sz->rnk - 1].is, vecsz->dims[0].is,
	       p->out, onembed, sz->dims[sz->rnk - 1].os, vecsz->dims[0].os,
	       k, flags);
	  bench_free(n); bench_free(inembed); bench_free(onembed);
	  goto done;
     }

 api_guru:
     {
	  FFTW(iodim) *dims, *howmany_dims;

	  dims = bench_tensor_to_fftw_iodim(sz, 1, 1);
	  howmany_dims = bench_tensor_to_fftw_iodim(vecsz, 1, 1);
	  if (verbose > 2) printf("using plan_guru_r2r\n");
	  pln = FFTW(plan_guru_r2r)(sz->rnk, dims,
				    vecsz->rnk, howmany_dims,
				    p->in, p->out, k, flags);
	  bench_free(dims);
	  bench_free(howmany_dims);
	  goto done;
     }
     
 done:
     bench_free(k);
     return pln;
}

static FFTW(plan) mkplan(bench_problem *p, int flags)
{
     switch (p->kind) {
	 case PROBLEM_COMPLEX:	  return mkplan_complex(p, flags);
	 case PROBLEM_REAL:	  return mkplan_real(p, flags);
	 case PROBLEM_R2R:        return mkplan_r2r(p, flags);
	 default: BENCH_ASSERT(0); return 0;
     }
}

int can_do(bench_problem *p)
{
     rdwisdom();
     if (p->destroy_input)
	  the_flags |= FFTW_DESTROY_INPUT;
     the_plan = mkplan(p, the_flags | FFTW_ESTIMATE);

     if (the_plan) {
	  FFTW(destroy_plan)(the_plan);
	  return 1;
     }
     return 0;
}


void setup(bench_problem *p)
{
     double tim;
     int save_flags = the_flags;

     if (p->destroy_input)
	  the_flags |= FFTW_DESTROY_INPUT;
     if (p->kind == PROBLEM_REAL && p->sign > 0 && !p->in_place)
	  p->destroy_input = 1; /* default for c2r out-of-place transforms */

#ifdef HAVE_THREADS
     BENCH_ASSERT(FFTW(init_threads)());
     FFTW(plan_with_nthreads)(nthreads);
#endif
     install_hook();

     rdwisdom();

     timer_start();
     the_plan = mkplan(p, the_flags);
     tim = timer_stop();
     if (verbose > 1) printf("planner time: %g s\n", tim);

     BENCH_ASSERT(the_plan);

     if (verbose > 1) {
	  int add, mul, fma;
	  FFTW(print_plan)(the_plan, stdout);
	  printf("\n");
	  FFTW(flops)(the_plan, &add, &mul, &fma);
	  printf("flops: %d add, %d mul, %d fma\n", add, mul, fma);
     }

     the_flags = save_flags;
}


void doit(int iter, bench_problem *p)
{
     int i;
     FFTW(plan) q = the_plan;

     UNUSED(p);
     for (i = 0; i < iter; ++i) 
	  FFTW(execute)(q);
}

void done(bench_problem *p)
{
     UNUSED(p);

     FFTW(destroy_plan)(the_plan);
     uninstall_hook();
}

void cleanup(void)
{
     wrwisdom();
     FFTW(cleanup)();

#    ifdef FFTW_DEBUG
     {
	  /* undocumented memory checker */
	  extern void FFTW(malloc_print_minfo)(int v);
	  if (verbose > 2)
	       FFTW(malloc_print_minfo)(verbose);
     }
#    endif

}



















/*------------------------------------------------------------*/
/* old test program, for reference purposes */
#ifdef OLD_TEST_PROGRAM

#include "bench-user.h"
#include <math.h>
#include <stdio.h>

extern void timer_start(void);
extern double timer_stop(void);

/*
  horrible hack for now.  This will go away once we define an interface
  for fftw
*/
#define problem fftw_problem
#include "ifftw.h"
#include "dft.h"
#include "rdft.h"
#include "reodft.h"
#include "threads.h"
void FFTW(dft_verify)(plan *pln, const problem_dft *p, int rounds);
void FFTW(rdft_verify)(plan *pln, const problem_rdft *p, int rounds);
void FFTW(reodft_verify)(plan *pln, const problem_rdft *p, int rounds);
#define FFTW X
#define fftw_real R
#undef problem

/* END HACKS */

static const char *mkvers(void)
{
     return FFTW(version);
}

static const char *mkcc(void)
{
     return FFTW(cc);
}

static const char *mkcodelet_optim(void)
{
     return FFTW(codelet_optim);
}

BEGIN_BENCH_DOC
BENCH_DOC("name", "fftw3")
BENCH_DOCF("version", mkvers) 
BENCH_DOCF("fftw-compiled-by", mkcc)
BENCH_DOCF("codelet-optim", mkcodelet_optim)
END_BENCH_DOC 

static bench_real *ri, *ii, *ro, *io;
static int is, os;

void copy_c2c_from(bench_problem *p, bench_complex *in)
{
     unsigned int i;
     if (p->sign == FFT_SIGN) {
	  for (i = 0; i < p->size; ++i) {
	       ri[i * is] = c_re(in[i]);
	       ii[i * is] = c_im(in[i]);
	  }
     } else {
	  for (i = 0; i < p->size; ++i) {
	       ii[i * is] = c_re(in[i]);
	       ri[i * is] = c_im(in[i]);
	  }
     }
}

void copy_c2c_to(bench_problem *p, bench_complex *out)
{
     unsigned int i;
     if (p->sign == FFT_SIGN) {
	  for (i = 0; i < p->size; ++i) {
	       c_re(out[i]) = ro[i * os];
	       c_im(out[i]) = io[i * os];
	  }
     } else {
	  for (i = 0; i < p->size; ++i) {
	       c_re(out[i]) = io[i * os];
	       c_im(out[i]) = ro[i * os];
	  }
     }
}

void copy_h2c(bench_problem *p, bench_complex *out)
{
     if (p->split)
	  copy_h2c_1d_halfcomplex(p, out, FFT_SIGN);
     else
	  copy_h2c_unpacked(p, out, FFT_SIGN);
}

void copy_c2h(bench_problem *p, bench_complex *in)
{
     if (p->split)
	  copy_c2h_1d_halfcomplex(p, in, FFT_SIGN);
     else
	  copy_c2h_unpacked(p, in, FFT_SIGN);
}

void copy_r2c(bench_problem *p, bench_complex *out)
{
     if (!p->split)
          copy_r2c_unpacked(p, out);
     else
          copy_r2c_packed(p, out);
}

void copy_c2r(bench_problem *p, bench_complex *in)
{
     if (!p->split)
          copy_c2r_unpacked(p, in);
     else
          copy_c2r_packed(p, in);
}

static void hook(plan *pln, const fftw_bench_problem *p_, int optimalp)
{
     UNUSED(optimalp);

     if (verbose > 5) {
	  printer *pr = FFTW(mkprinter_file) (stdout);
	  pr->print(pr, "%P:%(%p%)\n", p_, pln);
	  FFTW(printer_destroy) (pr);
	  printf("cost %g  \n\n", pln->pcost);
     }

     if (paranoid) {
	  if (DFTP(p_))
	       FFTW(dft_verify)(pln, (const problem_dft *) p_, 5);
	  else if (RDFTP(p_)) {
	       FFTW(rdft_verify)(pln, (const problem_rdft *) p_, 5);
	       FFTW(reodft_verify)(pln, (const problem_rdft *) p_, 5);
	  }
     }
}

int can_do(bench_problem *p)
{
     return (sizeof(fftw_real) == sizeof(bench_real) &&
	     (p->kind == PROBLEM_COMPLEX || p->kind == PROBLEM_REAL));
}

static planner *plnr;
static fftw_bench_problem *prblm;
static plan *pln;

void setup(bench_problem *p)
{
     double tplan;
     size_t nsize;

     BENCH_ASSERT(can_do(p));

     FFTW(threads_init)();

     plnr = FFTW(mkplanner)();
     FFTW(dft_conf_standard) (plnr);
     FFTW(rdft_conf_standard) (plnr);
     FFTW(reodft_conf_standard) (plnr);
     FFTW(threads_conf_standard) (plnr);
     plnr->nthr = 1;
     plnr->hook = hook;
     plnr->planner_flags |= NO_EXHAUSTIVE;
     plnr->planner_flags |= NO_LARGE_GENERIC;

     /* plnr->planner_flags |= IMPATIENT; */
     /* plnr->planner_flags |= ESTIMATE | IMPATIENT | NO_INDIRECT_OP; */

     if (p->kind == PROBLEM_REAL)
	  plnr->problem_flags |= DESTROY_INPUT;

#if 1
     {
	  FILE *f;
	  timer_start();
	  if ((f = fopen("wis.dat", "r"))) {
	       scanner *sc = FFTW(mkscanner_file)(f);
	       if (!plnr->adt->imprt(plnr, sc))
		    fprintf(stderr, "bench: ERROR reading wis.dat!\n");
	       FFTW(scanner_destroy)(sc);
	       fclose(f);
	  }

	  tplan = timer_stop();
	  {
               printer *pr = FFTW(mkprinter_file)(stdout);
	       if (verbose)
		    pr->print(pr, "READ WISDOM (%g seconds): ", tplan);
               if (verbose > 3)
		    plnr->adt->exprt(plnr, pr);
	       if (verbose)
		    pr->print(pr, "\n");
               FFTW(printer_destroy)(pr);
          }
     }
#endif

     if (p->kind == PROBLEM_REAL) {
	  if (p->split) {
	       is = os = 1;
	       ri = ii = p->in;
	       ro = io = p->out;
	  }
	  else if (p->sign == FFT_SIGN) {
	       is = 1; os = 2;
	       ri = ii = p->in;
	       ro = p->out; io = ro + 1;
	  }
	  else {
	       is = 2; os = 1;
	       ri = ii = p->out;
	       ro = p->in; io = ro + 1;
	  }
     }
     else if (p->split) {
	  is = os = 1;
	  if (p->sign == FFT_SIGN) {
	       ri = p->in;
	       ii = ri + p->size;
	       ro = p->out;
	       io = ro + p->size;
	  } else {
	       ii = p->in;
	       ri = ii + p->size;
	       io = p->out;
	       ro = io + p->size;
	  }
     } else {
	  is = os = 2;
	  if (p->sign == FFT_SIGN) {
	       ri = p->in;
	       ii = ri + 1;
	       ro = p->out;
	       io = ro + 1;
	  } else {
	       ii = p->in;
	       ri = ii + 1;
	       io = p->out;
	       ro = io + 1;
	  }
     }

     nsize = p->phys_size / p->vsize;
     if (p->kind == PROBLEM_COMPLEX)
	  prblm = 
	       FFTW(mkproblem_dft_d)(
		    FFTW(mktensor_rowmajor)(p->rank, p->n, p->n, p->n, is, os),
		    FFTW(mktensor_rowmajor) (p->vrank, p->vn, p->vn, p->vn,
					     is * nsize, os * nsize), 
		    ri, ii, ro, io);
     else if (p->split) {
	  CK(p->rank == 1);
	  prblm = 
	       FFTW(mkproblem_rdft_1_d)(
		    FFTW(mktensor_rowmajor)(p->rank, p->n, p->n, p->n, is, os),
		    FFTW(mktensor_rowmajor) (p->vrank, p->vn, p->vn, p->vn,
					     is * nsize, os * nsize), 
		    ri, ro, 
#if 1
		    p->sign == FFT_SIGN ? R2HC : HC2R
#else
                    RODFT00
#endif
		    ); /* emacs is confused if you duplicate the paren
			  inside #if */
     }
     else {
	  int i, *npadr, *npadc;
	  npadr = (int *) bench_malloc(p->rank * sizeof(int));
	  npadc = (int *) bench_malloc(p->rank * sizeof(int));
	  for (i = 0; i < p->rank; ++i) npadr[i] = npadc[i] = p->n[i];
	  if (p->rank > 0)
	       npadr[p->rank-1] = 2*(npadc[p->rank-1] = npadr[p->rank-1]/2+1);
	  prblm = 
	       FFTW(mkproblem_rdft2_d)(
		    p->sign == FFT_SIGN
		    ? FFTW(mktensor_rowmajor)(p->rank, p->n, npadr, npadc, 
					      is, os)
		    : FFTW(mktensor_rowmajor)(p->rank, p->n, npadc, npadr, 
					      is, os),
		    FFTW(mktensor_rowmajor) (p->vrank, p->vn, p->vn, p->vn,
					     is * nsize, (os * nsize) / 2), 
		    ri, ro, io, 
		    p->sign == FFT_SIGN ? R2HC : HC2R);
	  bench_free(npadc);
	  bench_free(npadr);
     }
     timer_start();
     pln = plnr->adt->mkplan(plnr, prblm);
     tplan = timer_stop();
     BENCH_ASSERT(pln);

     {
	  /* tentative blessing protocol (to be implemented by API) */
	  plan *pln0;
	  plnr->planner_flags |= BLESSING;
	  pln0 = plnr->adt->mkplan(plnr, prblm);
	  FFTW(plan_destroy_internal)(pln0);
	  plnr->planner_flags &= ~BLESSING;
     }
	  
     if (verbose) {
	  printer *pr = FFTW(mkprinter_file) (stdout);
	  pr->print(pr, "%p\nnprob %u  nplan %u\n",
		    pln, plnr->nprob, plnr->nplan);
	  pr->print(pr, "%d add, %d mul, %d fma, %d other\n",
		    pln->ops.add, pln->ops.mul, pln->ops.fma, pln->ops.other);
	  pr->print(pr, "planner time: %g s\n", tplan);
	  if (verbose > 1) {
	       plnr->adt->exprt(plnr, pr);
	       pr->print(pr, "\n");
	  }
	  FFTW(printer_destroy)(pr);
     }
     AWAKE(pln, 1);
#if 1
     if (pln)
          hook(pln, prblm, 1);
#endif
}

void doit(int iter, bench_problem *p)
{
     int i;
     plan *PLN = pln;
     fftw_bench_problem *PRBLM = prblm;

     UNUSED(p);
     for (i = 0; i < iter; ++i) {
	  PLN->adt->solve(PLN, PRBLM);
     }
}

void done(bench_problem *p)
{
     UNUSED(p);

#    ifdef FFTW_DEBUG
     if (verbose >= 2)
	  FFTW(planner_dump)(plnr, verbose - 2);
#    endif

     AWAKE(pln, 0);
     FFTW(plan_destroy_internal) (pln);
     FFTW(problem_destroy) (prblm);

#if 1
     {
	  FILE *f;
	  if ((f = fopen("wis.dat", "w"))) {
	       printer *pr = FFTW(mkprinter_file)(f);
	       plnr->adt->exprt(plnr, pr);
	       FFTW(printer_destroy)(pr);
	       fclose(f);
	  }
     }
#endif

     FFTW(planner_destroy) (plnr);

#    ifdef FFTW_DEBUG
     if (verbose >= 1)
	  FFTW(malloc_print_minfo)(verbose);
#    endif
}

#endif /* OLD_TEST_PROGRAM */
