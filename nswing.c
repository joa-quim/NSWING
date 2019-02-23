/*--------------------------------------------------------------------
 *	$Id: nswing.c 9936 2016-11-17 17:57:28Z j $
 *
 *	Copyright (c) 2012-2015 by J. Luis and J. M. Miranda
 *
 * 	This program is part of Mirone and is free software; you can redistribute
 * 	it and/or modify it under the terms of the GNU Lesser General Public
 * 	License as published by the Free Software Foundation; either
 * 	version 2.1 of the License, or any later version.
 * 
 * 	This program is distributed in the hope that it will be useful,
 * 	but WITHOUT ANY WARRANTY; without even the implied warranty of
 * 	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * 	Lesser General Public License for more details.
 *
 *	Contact info: w3.ualg.pt/~jluis/mirone
 *--------------------------------------------------------------------*/

static char prog_id[] = "$Id: nswing.c 9936 2016-11-17 17:57:28Z j $";

/*
 *	Original Fortran version of core hydrodynamic code by J.M. Miranda and COMCOT
 *
 *
 *		The nesting grids algorithm 
	LOOP_OVER_ALL_CYCLES {
		mass_L0
		openbd
		LOOP_OVER_N_NESTED {
			LOOP_TIME_REFINE_1 {
	 			interp_edges_L1
 				mass_L1
				LOOP_TIME_REFINE_2 {
					interp_edges_L2
					mass_L2
					LOOP_TIME_REFINE_3 {
						...
					}
					moment_L2
					upscale_L2
					update_L2
				}
				moment_L1
				upscale_L1
				update_L1
			}
		}
		moment_L0
		update_L0
	}
 *
 *
 * To compile, do for example
 *	cl nswing.c -IC:\programs\compa_libs\netcdf_GIT\compileds\VC12_64\include
 *     C:\programs\compa_libs\netcdf_GIT\compileds\VC12_64\lib\netcdf.lib /DI_AM_C 
 *     /DHAVE_NETCDF /nologo /D_CRT_SECURE_NO_WARNINGS /fp:precise /Ox
 *
 *	Rewritten in C, mexified, added number options, etc... By
 *	Joaquim Luis - 2013
 *
 */

#if defined(WIN32) || defined(_WIN32) || defined(_WIN64)
#	define DO_MULTI_THREAD	/* ISTO TEM DE SER AUTOMATIZADO, OU VIA COMPILA */
#else
#	define strtok_s strtok_r
#endif

#define I_AM_MEX        /* Build as a MEX */

#ifdef I_AM_C           /* Build as a stand-alone exe */
#	undef I_AM_MEX
#endif

#include <float.h>
#include <math.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#ifdef I_AM_MEX
#	include "mex.h"
#	include "mwsize.h"
#else
#	include <stdio.h>
#	include <stdlib.h>
#	include <stdint.h>
#	define mxCalloc calloc
#	define mxMalloc malloc
#	define mxRealloc realloc
#	define mxFree free
#	define mexPrintf(...) fprintf(stderr, __VA_ARGS__);

#	ifndef NAN
#		ifdef _MSC_VER
#			include <ymath.h>
#			define NAN _Nan._Double
#		else
			static const double _NAN = 20;//(HUGE_VAL-HUGE_VAL);
#			define NAN _NAN
#		endif
#	endif
#	define mxGetNaN() (NAN)

#endif

/* At least the Intel compiler screws anf the NAN macro returns 0.0 So we need this hack */
/* http://stackoverflow.com/questions/5714131/nan-literal-in-c */
union {uint64_t i; double d;} loc_nan = {0x7ff8000000000000};

#ifdef HAVE_NETCDF
#	include <netcdf.h>
#	define err_trap(status) if (status) {mexPrintf ("NSWING: error at line: %d\t and errorcode = %s\n", __LINE__, nc_strerror(status));}
#endif

#if defined(WIN32) || defined(_WIN32) || defined(_WIN64)
#	include <windows.h>
#	include <process.h>
#endif

#if HAVE_OPENMP
#include <omp.h>
#endif

#define	FALSE	0
#define	TRUE	1
#ifndef M_PI
#	define M_PI	3.14159265358979323846
#endif
#ifndef M_PI_2
#	define M_PI_2  1.57079632679489661923
#endif
#define D2R		M_PI / 180.
#define R2D		180. / M_PI

#define GMT_CONV_LIMIT  1.0e-8      /* Fairly tight convergence limit or "close to zero" limit */ 
#define	NORMAL_GRAV	9.806199203     /* Moritz's 1980 IGF value for gravity at 45 degrees latitude */
#define	EQ_RAD 6378137.0            /* WGS-84 */
#define	flattening  1.0/298.2572235630
#define LIMIT_DISCHARGE			/* If defined the moment functions will limit the discharge */
#define ECC2  2 * flattening - flattening * flattening
#define ECC4  ECC2 * ECC2
#define ECC6  ECC2 * ECC4
#define EPS12 1e-12
#define EPS10 1e-10
#define EPS6 1e-6
#define EPS5 1e-5
#define EPS4_ 1e-4
#define EPS3 1e-3
#define EPS2 1e-2
#define EPS1 1e-1

static double EPS4 = EPS4_;		/* Kinda trick to be able to change EPS4 via a command line option */

#define MAXRUNUP -50 	/* Do not waste time computing flood above this altitude */
#define V_LIMIT   20	/* Upper limit of maximum velocity */

#define CNULL	((char *)NULL)
#define Loc_copysign(x,y) ((y) < 0.0 ? -fabs(x) : fabs(x))

#ifndef MIN
#	define MIN(x, y) (((x) < (y)) ? (x) : (y))	/* min and max value macros */
#endif
#ifndef MAX
#	define MAX(x, y) (((x) > (y)) ? (x) : (y))
#endif

#ifndef rint
#	define rint(x) (floor((x)+0.5))
#endif
#ifndef irint
#	define irint(x) ((int)rint(x))
#endif

#define ijs(i,j,n) ((i) + (j)*n)
#define ijc(i,j) ((i) + (j)*n_ptmar)
#define ij_grd(col,row,hdr) ((col) + (row)*hdr.nx)

typedef void (*PFV) ();		/* PFV declares a pointer to a function returning void */

struct tracers {        /* For tracers (oranges) */
	double *x;          /* x coordinate */
	double *y;          /* y coordinate */
};

struct srf_header {     /* Surfer file hdr structure */
	char id[4];         /* ASCII Binary identifier (DSAA/DSBB) */
	short int nx;       /* Number of columns */
	short int ny;       /* Number of rows */
	double x_min;       /* Minimum x coordinate */
	double x_max;       /* Maximum x coordinate */
	double y_min;       /* Minimum y coordinate */
	double y_max;       /* Maximum y coordinate */
	double z_min;       /* Minimum z value */
	double z_max;       /* Maximum z value */
};

struct grd_header {     /* Generic grid hdr structure */
	int nx;             /* Number of columns */
	int ny;             /* Number of rows */
	unsigned int nm;    /* nx * ny */
	double x_inc;
	double y_inc;
	double x_min;       /* Minimum x coordinate */
	double x_max;       /* Maximum x coordinate */
	double y_min;       /* Minimum y coordinate */
	double y_max;       /* Maximum y coordinate */
	double z_min;       /* Minimum z value */
	double z_max;       /* Maximum z value */

	int doCoriolis;		/* Apply Coriolis if this != 0 */
	double lat_min4Coriolis;	/* PRECISA SOLUCAO. POR AGORA SERA Cte = 0 */
};

struct nestContainer {         /* Container for the nestings */
	int    do_upscale;         /* If false, do not upscale the parent grid */
	int    do_long_beach;      /* If true, compute a mask with ones over the "dryed beach" */
	int    do_short_beach;     /* If true, compute a mask with ones over the "innundated beach" */
	int    do_linear;          /* If true, use linear approximation */
	int    do_max_level;       /* If true, inform nestify() on the need to update max level at every inner iteration */
	int    do_max_velocity;    /* If true, inform nestify() on the need to update max velocity at each inner iteration */
	int    do_Coriolis;        /* If true, compute the Coriolis effect */
	int    out_velocity_x;     /* To know if we must compute the vex,vey velocity arrays */
	int    out_velocity_y;
	int    out_momentum;       /* To know if save the momentum in the 3D netCDF grid. Mutually exclusive with out_velocity_x|y */
	int    isGeog;             /* 0 == Cartesian, otherwise Geographic coordinates */
	int    writeLevel;         /* Store info about which level is (if) to be writen [0] */
	int    bnc_pos_nPts;       /* Number of points in a external boundary condition file */
	int    bnc_var_nTimes;     /* Number of time steps in the external boundary condition file */
	int    bnc_border[4];      /* Each will be set to TRUE if boundary condition on that border W->0, S->1, E->2, N->3 */
	int    level[10];          /* 0 Will mean base level, others the nesting level */
	int    LLrow[10], LLcol[10], ULrow[10], ULcol[10], URrow[10], URcol[10], LRrow[10], LRcol[10];
	int    incRatio[10];
	short  *long_beach[10];    /* Mask arrays for storing the "dry beaches" */
	short  *short_beach[10];   /* Mask arrays for storing the "dry beaches" */
	float  *work, *wmax;       /* Auxiliary pointers (not direcly allocated) to compute max level of nested grids */
	float  *vmax;              /* Pointer to array storing the max velocity */
	double run_jump_time;      /* Time to hold before letting the nested grids start to iterate */
	double lat_min4Coriolis;   /* South latitute when computing the Coriolis effect on a cartesian grid */
	double manning_depth;      /* Do not use manning if depth is deeper than this value */
	double manning[10];        /* Manning coefficient. Set to zero if no friction */
	double LLx[10], LLy[10], ULx[10], ULy[10], URx[10], URy[10], LRx[10], LRy[10];
	double dt[10];                             /* Time step at current level               */
	double *bat[10];                           /* Bathymetry of current level              */
	double *fluxm_a[10],  *fluxm_d[10];        /* t-1/2 & t+1/2 fluxes arrays along X      */
	double *fluxn_a[10],  *fluxn_d[10];        /* t-1/2 & t+1/2 fluxes arrays along Y      */
	double *htotal_a[10], *htotal_d[10];       /* t-1/2 & t+1/2 total water depth         */
	double *vex[10],  *vey[10];                /* X,Y velocity components                  */
	double *etaa[10], *etad[10];               /* t-1/2 & t+1/2 water height (eta) arrays */
	double *edge_col[10], *edge_colTmp[10];
	double *edge_row[10], *edge_rowTmp[10];
	double *edge_col_P[10], *edge_col_Ptmp[10];
	double *edge_row_P[10], *edge_row_Ptmp[10];
	double *r0[10], *r1m[10], *r1n[10], *r2m[10], *r2n[10], *r3m[10], *r3n[10], *r4m[10], *r4n[10];
	double time_h;
	double *bnc_pos_x;
	double *bnc_pos_y;
	double *bnc_var_t;
	double **bnc_var_z;
	double *bnc_var_zTmp;
	double *bnc_var_z_interp;
	struct grd_header hdr[10];
};

/* Argument struct for threading */
typedef struct {
	struct nestContainer *nest;   /* Pointer to a nestContainer struct */
	int iThread;                  /* Thread index */
	int lev;                      /* Level of nested grid */
} ThreadArg;

void no_sys_mem(char *where, unsigned int n);
int  count_col(char *line);
int  read_grd_info_ascii(char *file, struct srf_header *hdr);
int  read_header_bin (FILE *fp, struct srf_header *hdr);
int  write_grd_bin(char *name, double x_min, double y_min, double x_inc, double y_inc, unsigned int i_start, 
                   unsigned int j_start, unsigned int i_end, unsigned int j_end, unsigned int nX, float *work);
int  read_grd_ascii (char *file, struct srf_header *hdr, double *work, int sign);
int  read_grd_bin(char *file, struct srf_header *hdr, double *work, int sign);
int  read_maregs(struct grd_header hdr, char *file, unsigned int *lcum_p, char *names[]);
int  read_tracers(struct grd_header hdr, char *file, struct tracers *oranges);
int  count_n_maregs(char *file);
int  decode_R(char *item, double *w, double *e, double *s, double *n);
int  check_region(double w, double e, double s, double n);
double ddmmss_to_degree (char *text);
void openb(struct grd_header hdr, double *bat, double *fluxm_d, double *fluxn_d, double *etad, struct nestContainer *nest);
void wave_maker(struct nestContainer *nest);
void wall_it(struct nestContainer *nest);
void wall_two(struct nestContainer *nest, int ot1, int ot2, int in1, int in2);
int  initialize_nestum(struct nestContainer *nest, int isGeog, int lev);
int  intp_lin (double *x, double *y, int n, int m, double *u, double *v);
void inisp(struct nestContainer *nest);
void inicart(struct nestContainer *nest);
void interp_edges(struct nestContainer *nest, double *flux_L1, double *flux_L2, char *what, int lev, int i_time);
void sanitize_nestContainer(struct nestContainer *nest);
void nestify(struct nestContainer *nest, int nNg, int recursionLevel, int isGeog);
void resamplegrid(struct nestContainer *nest, int nNg);
void edge_communication(struct nestContainer *nest, int lev, int i_time);
void mass(struct nestContainer *nest, int lev);
void mass_sp(struct nestContainer *nest, int lev);
void mass_conservation(struct nestContainer *nest, int isGeog, int m);
void moment_conservation(struct nestContainer *nest, int isGeog, int m);
void update(struct nestContainer *nest, int lev);
void upscale(struct nestContainer *nest, double *out, int lev, int i_tsr);
void upscale_(struct nestContainer *nest, double *out, int lev, int i_tsr);
void replicate(struct nestContainer *nest, int lev);
void moment_M(struct nestContainer *nest, int lev);
void moment_N(struct nestContainer *nest, int lev);
void moment_sp_M(struct nestContainer *nest, int lev);
void moment_sp_N(struct nestContainer *nest, int lev);
void free_arrays(struct nestContainer *nest, int isGeog, int lev);
int  check_paternity(struct nestContainer *nest);
int  check_binning(double x0P, double x0D, double dxP, double dxD, double tol, double *suggest);
int  read_bnc_file(struct nestContainer *nest, char *file);
int  interp_bnc (struct nestContainer *nest, double t);
void total_energy(struct nestContainer *nest, float *work, int lev);
void power(struct nestContainer *nest, float *work, int lev);
void vtm (double lat0, double *t_c1, double *t_c2, double *t_c3, double *t_c4, double *t_e2, double *t_M0);
void deform (struct srf_header hdr, double x_inc, double y_inc, int isGeog, double fault_length,
             double fault_width, double th, double dip, double rake, double d, double top_depth,
             double xl, double yl, double *z);
void kaba_source(struct srf_header hdr, double x_inc, double y_inc, double x_min, double x_max,
	             double y_min, double y_max, int type, double *z);
void tm (double lon, double lat, double *x, double *y, double central_meridian, double t_c1,
         double t_c2, double t_c3, double t_c4, double t_e2, double t_M0);
double uscal(double x1, double x2, double x3, double c, double cc, double dp);
double udcal(double x1, double x2, double x3, double c, double cc, double dp);
unsigned int gmt_bcr_prep (struct grd_header hdr, double xx, double yy, double wx[], double wy[]);
double GMT_get_bcr_z(double *grd, struct grd_header hdr, double xx, double yy);
void update_max(struct nestContainer *nest);
void update_max_velocity(struct nestContainer *nest);


#ifdef HAVE_NETCDF
void write_most_slice(struct nestContainer *nest, int *ncid_most, int *ids_most, unsigned int i_start,
                      unsigned int j_start, unsigned int i_end, unsigned int j_end, float *work, size_t *start,
                      size_t *count, double *slice_range, int isMost, int lev);
int open_most_nc(struct nestContainer *nest, float *work, char *basename, char *name_var, char hist[], int *ids,
                 unsigned int nx, unsigned int ny, double xMinOut, double yMinOut, int isMost, int lev);
int open_anuga_sww(struct nestContainer *nest, char *fname_sww, char history[], int *ids, unsigned int i_start,
                   unsigned int j_start, unsigned int i_end, unsigned int j_end, double xMinOut, double yMinOut, int lev);
void write_anuga_slice(struct nestContainer *nest, int ncid, int z_id, unsigned int i_start, unsigned int j_start,
                       unsigned int i_end, unsigned int j_end, float *work, size_t *start, size_t *count,
                       float *slice_range, int idx, int with_land, int lev);
int write_maregs_nc(struct nestContainer *nest, char *fname, float *work, double *t, unsigned int *lcum_p,
                    char *names[], char hist[], int n_maregs, unsigned int count_time_maregs_timeout, int lev);
int write_greens_nc(struct nestContainer *nest, char *fname, float *work, size_t *start, size_t *count,
                    double *t, unsigned int *lcum_p, char *names[], char hist[], int *ids, int n_maregs,
                    unsigned int n_times, int lev);
void err_trap_(int status);
#endif

#if defined(WIN32) || defined(_WIN32) || defined(_WIN64)
/* Prototypes for threading related functions */
unsigned __stdcall MT_cart(void *Arg_p);
unsigned __stdcall MT_sp(void *Arg_p);
int GetLocalNThread(void);
#endif

/* Function pointers to M & N moment components */
PFV call_moment[2];
PFV call_moment_sp[2];

int Return(int code) {		/* To handle return codes between MEX and standalone code */
#ifdef I_AM_MEX
	mexErrMsgTxt("\n");		/* Most of the cases (no_sys_mem) this instruction was executed already */
#endif
	return(code);
}

/* --------------------------------------------------------------------------- */
/* Matlab Gateway routine */

#ifdef I_AM_MEX
#	define Return(code) {mexErrMsgTxt("\n");}
void mexFunction(int nlhs, mxArray *plhs[], int nrhs, const mxArray *prhs[]) {
#else
#	define Return(code) {return(code);}
int main(int argc, char **argv) {
#endif

	int     writeLevel = 0;              /* If save grids, will hold the saving level (when nesting) */
	int     i, j, k, n;
	int     start_i;                     /* Where the loop over argc starts: MEX -> 0; STANDALONE -> 1 */
	int     grn = 0, cumint = 0, decimate_max = 1, iprc, r_bin_b, r_bin_f, r_bin_mM, r_bin_mN;
	int     w_bin = TRUE, cumpt = FALSE, error = FALSE, do_2Dgrids = FALSE, do_maxs = FALSE;
	int     out_energy = FALSE, max_energy = FALSE, out_power = FALSE, max_power = FALSE;
	int     first_anuga_time = TRUE, out_sww = FALSE, out_most = FALSE, out_3D = FALSE;
	int     surf_level = TRUE, max_level = FALSE, max_velocity = FALSE, water_depth = FALSE;
	int     do_Okada = FALSE;            /* For when one will compute the Okada deformation here */
	int     do_Kaba = FALSE;             /* For when one will use prismatic sources */
	int     do_tracers = FALSE;          /* For when doing Lagrangian tracers */
	int     out_maregs_nc = FALSE;       /* For when maregs in output are written in netCDF */
	int     out_oranges_nc = FALSE;      /* For when tracers in output are written in netCDF */
	int     do_HotStart = FALSE;         /* For when doing a Hot Start */
	int     n_arg_no_char = 0;
	int     ncid, ncid_most[3], z_id = -1, ids[13], ids_ha[6], ids_ua[6], ids_va[6], ids_most[3];
	int     ncid_3D[3], ids_z[10], ids_3D[3], ncid_Mar, ids_Mar[8];
	int     n_of_cycles = 1010;          /* Default number of cycles to compute */
	int     num_of_nestGrids = 0;        /* Number of nesting grids */
	int     bat_in_input = FALSE, source_in_input = FALSE, write_grids = FALSE, isGeog = FALSE;
	int     maregs_in_input = FALSE, out_momentum = FALSE, got_R = FALSE;
	int     with_land = FALSE, IamCompiled = FALSE, do_nestum = FALSE, saveNested = FALSE, verbose = FALSE;
	int     out_velocity = FALSE, out_velocity_x = FALSE, out_velocity_y = FALSE, out_velocity_r = FALSE;
	int     out_maregs_velocity = FALSE;
	int     KbGridCols = 1, KbGridRows = 1; /* Number of rows & columns IF computing a grid of 'Kabas' */
	int     cntKabas = 0;                /* Counter of the number of Kabas (prisms) already processed */
	int     n_mareg, n_ptmar, n_oranges, pos_prhs;
	unsigned int *lcum_p = NULL, lcum = 0, ij, nx, ny;
	unsigned int i_start, j_start, i_end, j_end, count_maregs_timeout = 0, count_time_maregs_timeout = 0;
	size_t	start0 = 0, count0 = 1, len, start1_A[2] = {0,0}, count1_A[2];
	size_t  start1_M[3] = {0,0,0}, count1_M[3], start_Mar[3] = {0,0,0}, count_Mar[2];
	char   *bathy   = NULL;              /* Name pointer for bathymetry file */
	char   	hcum[256]   = "";            /* Name of the cumulative hight file */
	char    maregs[256] = "";            /* Name of the maregraph positions file */
	char  **mareg_names = NULL;          /* Array to hold optional maregraph names */
	char   *fname_sww = NULL;            /* Name pointer for Anuga's .sww file */
	char   *basename_most = NULL;        /* Name pointer for basename of MOST .nc files */
	char   *fname3D  = NULL;             /* Name pointer for the 3D netCDF file */
	char   *fonte    = NULL;             /* Name pointer for tsunami source file */
	char   *bnc_file = NULL;             /* Name pointer for a boundary condition file */
	char    fname_mask_lbeach[256] = ""; /* Name pointer for the "long_beach" mask grid */
	char    fname_mask_sbeach[256] = ""; /* Name pointer for the "short_beach" mask grid */
	char    tracers_infile[256] = "", tracers_outfile[256] = "";	/* Names for in and out tracers files */
	char    stem[256] = "", prenome[128] = "", str_tmp[128] = "", fname_momentM[256] = "", fname_momentN[256] = "";
	char    history[512] = {""};         /* To hold the full command call to be saved in nc files as History */
	char   *pch;
	char   *nesteds[10] = {NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL};
	char    txt[128];                    /* Auxiliary variable */

	float  *work = NULL, *workMax = NULL, *vmax = NULL, *wmax = NULL, *time_p = NULL;
	float   work_min = FLT_MAX, work_max = -FLT_MAX, *maregs_array = NULL, *maregs_array_t = NULL;
	double *maregs_timeout = NULL, m_per_deg = 111317.1;
	double *bat = NULL, *dep1 = NULL, *dep2 = NULL, *cum_p = NULL, *h = NULL;
	double  dfXmin = 0, dfYmin = 0, dfXmax = 0, dfYmax = 0, xMinOut, yMinOut;
	double  kaba_xmin = 0, kaba_xmax = 0, kaba_ymin = 0, kaba_ymax = 0;
	double  time_jump = 0, time0, time_for_anuga, prc;
	double  dt = 0;                     /* Time step for Base level grid */
	double  dx, dy, ds, dtCFL, etam, one_100, t;
	double *eta_for_maregs, *vx_for_maregs, *vy_for_maregs, *htotal_for_maregs, *fluxm_for_maregs, *fluxn_for_maregs;
	double *vx_for_oranges, *vy_for_oranges, *fluxm_for_oranges, *fluxn_for_oranges, *htotal_for_oranges;	/* For tracers */
	double  f_dip, f_azim, f_rake, f_slip, f_length, f_width, f_topDepth, x_epic, y_epic;	/* For Okada initial condition */
	double  add_const = 0, time_h = 0;
	double  dxKb = 0, dyKb = 0;         /* Grid steps for when computing a grid of 'Kabas' */
	double  z_offset = 0;	/* To apply to bathymetry to simulate a tide */
	double  manning[10] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};	/* Manning coefficients */

	double  actual_range[6] = {1e30, -1e30, 1e30, -1e30, 1e30, -1e30};
	float	stage_range[2], xmom_range[2], ymom_range[2], *tmp_slice;
	struct	srf_header hdr_b, hdr_f, hdr_mM, hdr_mN;
	struct	grd_header hdr;
	struct  nestContainer nest;
	struct  tracers *oranges;
	FILE   *fp, *fp_oranges;
#ifdef I_AM_MEX
	int     argc;
	unsigned nm;
	char   **argv, cmd[16] = "";
	double  *head, *tmp, x_inc, y_inc, x, y;
	double  *ptr_wb;                    /* Pointer to be used in the aguentabar */
	mxArray *rhs[3];
#endif
	clock_t tic;

	call_moment[0] = (PFV)moment_M;
	call_moment[1] = (PFV)moment_N;
	call_moment_sp[0] = (PFV)moment_sp_M;
	call_moment_sp[1] = (PFV)moment_sp_N;

#ifdef DO_MULTI_THREAD
	if ((k = GetLocalNThread()) == 1) {
		mexPrintf("NSWING: This version of the program is build for multi-threading but "
		           "this machine has only one core. Don't know what will happen here.\n"); 
	}
#endif

	sanitize_nestContainer(&nest);

#ifdef I_AM_MEX
	argc = nrhs;
	for (i = 0; i < nrhs; i++) {		/* Check input to find how many arguments are of type char */
		if (!mxIsChar(prhs[i])) {
			argc--;
			n_arg_no_char++;            /* Number of arguments that have a type other than char */
		}
	}

	if (n_arg_no_char >= 4) {          /* Not all combinations are tested */
		/* Bathymetry */
		dep1 = mxGetPr(prhs[0]);
		nx = mxGetN(prhs[0]);
		ny = mxGetM(prhs[0]);
		head  = mxGetPr(prhs[1]);	/* Get bathymetry header info */
		hdr_b.x_min = head[0];		hdr_b.x_max = head[1];
		hdr_b.y_min = head[2];		hdr_b.y_max = head[3];
		hdr_b.z_min = head[4];		hdr_b.z_max = head[5];
		hdr_b.nx = nx;				hdr_b.ny = ny;
		x_inc = head[7];			y_inc = head[8];
		bat_in_input = TRUE;
		/* Tsunami source */
		h = mxGetPr(prhs[2]);
		nx = mxGetN(prhs[2]);
		ny = mxGetM(prhs[2]);
		head  = mxGetPr(prhs[3]);	/* Get bathymetry header info */
		hdr_f.x_min = head[0];		hdr_f.x_max = head[1];
		hdr_f.y_min = head[2];		hdr_f.y_max = head[3];
		hdr_f.z_min = head[4];		hdr_f.z_max = head[5];
		hdr_f.nx = nx;				hdr_f.ny = ny;
		source_in_input = TRUE;
		/* Now test if bat & source are compatible. If they are not, we go out right here. */
		if (hdr_f.nx != hdr_b.nx || hdr_f.ny != hdr_b.ny) {
			mexPrintf("Bathymetry & Source grids have different nx and/or ny\n"); 
			mexPrintf("%d %d %d %d\n", hdr_f.nx, hdr_b.nx, hdr_f.ny, hdr_b.ny); 
			return;
		}
	}

	if (n_arg_no_char >= 5 && mxIsCell(prhs[4])) {
		const mxArray *mx_ptr;

		num_of_nestGrids = mxGetNumberOfElements(prhs[4]);
		if (num_of_nestGrids % 2 != 0)
			mexErrMsgTxt("NSWING: Cell array with nested grids is not a even number of elements");
		num_of_nestGrids /= 2;

		for (k = 0; k < num_of_nestGrids; k++) {
			mx_ptr = mxGetCell(prhs[4], k);
			if (mx_ptr == NULL)
				mexErrMsgTxt("NSWING: bloody cell element is empty!!!!!!!");
			dep2 = mxGetPr(mx_ptr);		/* Get the grid for this level */
			nest.hdr[k+1].nx = mxGetN(mx_ptr);
			nest.hdr[k+1].ny = mxGetM(mx_ptr);
			nest.hdr[k+1].nm = (unsigned int)nest.hdr[k+1].nx * (unsigned int)nest.hdr[k+1].ny;
			mx_ptr = mxGetCell(prhs[4], k + num_of_nestGrids);
			head  = mxGetData(mx_ptr);	/* Get header info */
			nest.hdr[k+1].x_min = head[0];		nest.hdr[k+1].x_max = head[1];
			nest.hdr[k+1].y_min = head[2];		nest.hdr[k+1].y_max = head[3];
			nest.hdr[k+1].z_min = head[4];		nest.hdr[k+1].z_max = head[5];
			nest.hdr[k+1].x_inc = head[7];		nest.hdr[k+1].y_inc = head[8];

			nm = nest.hdr[k+1].nx * nest.hdr[k+1].ny;
			if ((nest.bat[k+1] = (double *)mxCalloc((size_t)nm, sizeof(double)) ) == NULL) 
				{no_sys_mem("(bat)", nm); Return(-1);}
			for (i = 0; i < nest.hdr[k+1].ny; i++) {
				for (j = 0; j < nest.hdr[k+1].nx; j++)
					nest.bat[k+1][i*nest.hdr[k+1].nx+j] = -dep2[j*nest.hdr[k+1].ny+i];
			}
		}
		do_nestum = TRUE;
	}

	/* See if we have a vector of maregraphs */
	pos_prhs = 0;
	if (n_arg_no_char == 5 && !mxIsEmpty(prhs[4]))
		pos_prhs = 4;
	else if (n_arg_no_char == 6 && !mxIsEmpty(prhs[5])) 
		pos_prhs = 5;

	if (pos_prhs) {
		double x_min, x_max, y_min, y_max;
		tmp = mxGetPr(prhs[pos_prhs]);
		n_mareg = mxGetM(prhs[pos_prhs]);
		if (do_nestum) {
			dx = nest.hdr[num_of_nestGrids].x_inc;      dy = nest.hdr[num_of_nestGrids].y_inc;
			x_min = nest.hdr[num_of_nestGrids].x_min;   x_max = nest.hdr[num_of_nestGrids].x_max;
			y_min = nest.hdr[num_of_nestGrids].y_min;   y_max = nest.hdr[num_of_nestGrids].y_max;
			nx    = nest.hdr[num_of_nestGrids].nx;
		}
		else {
			dx    = x_inc;           dy    = x_inc;
			x_min = hdr_b.x_min;     x_max = hdr_b.x_max;
			y_min = hdr_b.y_min;     y_max = hdr_b.y_max;
			nx    = hdr_b.nx;
		}
		lcum_p = (unsigned int *)mxCalloc((size_t)(n_mareg), sizeof(unsigned int));
		mareg_names = mxCalloc((size_t)(n_mareg), sizeof(char *));
		for (ij = j = 0; ij < n_mareg; ij++) {
			x = tmp[ij];		y = tmp[ij+n_mareg];	/* Matlab vectors are stored by columns */
			if (x < x_min || x > x_max || y < y_min || y > y_max)
				continue;
			lcum_p[j] = (unsigned int)(irint((y - y_min) / dy) ) * nx + (unsigned int)irint((x - x_min) / dx);
			j++;
		}
		if (j > 0) {
			maregs_in_input = TRUE;
			cumpt = TRUE;
		}
		else {
			mexPrintf("NSWING: Warning, none of the provided maregraph locations lies inside (inner?) grid. Ignoring them\n");
			mxFree(lcum_p);
			lcum_p = NULL;
		}
	}

	/* get the length of the input string */
	argv = (char **)mxCalloc(argc, sizeof(char *));
	for (i = 0; i < argc; i++)
		argv[i] = (char *)mxArrayToString(prhs[i+n_arg_no_char]);

	start_i = 0;
#else
	start_i = 1;	/* For stand-alone first index is prog name, so we want to start at 1 */
#endif		/* end #ifdef I_AM_MEX */

	for (i = start_i; i < argc; i++) {
		if (argv[i][0] == '-') {
			switch (argv[i][1]) {
				case 'c':
					add_const = atof(&argv[i][2]);
					break;
				case 'e':
					IamCompiled = TRUE;
					break;
				case 'f':	/* */
					isGeog = TRUE;
					break;
				case 'n':	/* Write MOST files (*.nc) */
					basename_most  = &argv[i][2];
					out_most = TRUE;
					break;
				case 'A':	/* Name for the Anuga .sww netCDF file */
					fname_sww  = &argv[i][2];
					out_sww = TRUE;
					if (argv[i][2] == 'l')		/* Output land nodes in SWW file */
						with_land = TRUE;
					break;
				case 'B':	/* File with the boundary condition (experimental) */
					bnc_file = &argv[i][2];
					break;
				case 'C':	/* Output momentum grids */
					nest.do_Coriolis = TRUE;
					if (argv[i][2])
						nest.lat_min4Coriolis = atof(&argv[i][2]);
					break;
				case 'D':
					water_depth = TRUE;
					surf_level = FALSE;
					break;
				case 'E':	/* Compute total Energy or Power*/
					if (strstr(argv[i], "EPS4=")) {		/* Secreet option to change the EPS4 value */
						EPS4 = atof(&argv[i][6]);
						break;
					}
					if (argv[i][2] == 'p') {
						if (argv[i][3] == 'm')
							max_power = TRUE;
						else
							out_power = TRUE;
					}
					else {
						if (argv[i][2] == 'm')
							max_energy = TRUE;
						else
							out_energy = TRUE;
					}
					if ((pch = strstr(&argv[i][2],",")) != NULL) {
						decimate_max = atoi(++pch);
						if (decimate_max < 1) decimate_max = 1;
					}
					break;
				case 'F':	/* Okada parameters to compute Initial condition */
					if (argv[i][2] == 'k') {
						char *lost_str1 = NULL, *lost_str2 = NULL;
						int   have_RC = FALSE;

						do_Kaba = TRUE;
						k = 3;
						if (argv[i][3] == 'c') {
							k = 4;
							do_Kaba++;		/* Signal kaba_source function that the above is actually x/y/nx/ny */
						}
						n = sscanf(&argv[i][k], "%lf/%lf/%lf/%lf/%s/%lf", &kaba_xmin, &kaba_xmax, &kaba_ymin, &kaba_ymax, txt, &dyKb);
						if (n > 4) {
							lost_str1 = strdup(txt);		/* Save it to restore argv[i] for use in the nc History attrib */
							pch = strchr(txt, 'x');
							if (pch != NULL) {
								KbGridCols = atoi(&pch[1]);
								pch[0] = '\0';		/* Strip the 'x...' part */
								KbGridRows = atoi(txt);
								have_RC = TRUE;
							}
							else {
								dxKb = atof(txt);
								if (n == 5) dyKb = dxKb;
							}
							/* Now strip the 5th (&6th) fields from argv[i] before calling decode_R */
							if (n == 6) {
								pch = strrchr(argv[i], '/');
								lost_str2 = strdup(pch);	/* Iden lost_str1 */
								pch[0] = '\0';
							}
							pch = strrchr(argv[i], '/');		pch[0] = '\0';
						}

						k -= 2;	/* Decrease k because the decode_R() function expects 2 chars at begining of string (-R...) */
						error += decode_R(&argv[i][k], &kaba_xmin, &kaba_xmax, &kaba_ymin, &kaba_ymax);
						if (have_RC) {
							dxKb = (kaba_xmax - kaba_xmin);		dyKb = (kaba_ymax - kaba_ymin);
						}
						if (dxKb != 0 && !have_RC) {		/* Here dx/dy were given instead of RxC, so we have to compute them */
							KbGridCols = irint((kaba_xmax - kaba_xmin) / dxKb);
							KbGridRows = irint((kaba_ymax - kaba_ymin) / dyKb);
						}
						if (KbGridRows * KbGridCols > 1) out_maregs_nc = TRUE;	/* Otherwise we can still save in ascii */
						if (n > 4) {
							strcat(argv[i], "/");	strcat(argv[i], lost_str1);
							mxFree(lost_str1);
						}
						if (lost_str2) {
							strcat(argv[i], lost_str2);		/* Here the leading slash was still in lost_str2 */
							mxFree(lost_str2);
						}
					}
					else {
						do_Okada = TRUE;
						n = sscanf(&argv[i][2], "%lf/%lf/%lf/%lf/%lf/%lf/%lf/%lf/%lf", 
						           &f_dip, &f_azim, &f_rake, &f_slip, &f_length, &f_width, &f_topDepth, &x_epic, &y_epic);
						if (n != 9) {
							mexPrintf("NSWING: Error, -F option, must provide all 9 parameters.\n");
							error++;
						}
						else { /* Convert fault dimensions to meters (that's what is used by deform) */
							f_length   *= 1000;
							f_width    *= 1000;
							f_topDepth *= 1000;
						}
					}
					break;
				case 'G':	/* Write grids at grn intervals */
				case 'Z':	/* Write one single 3D netCDF at grn intervals */
					sscanf(&argv[i][2], "%s,%d", stem, &grn);
					if ((pch = strstr(stem,",")) != NULL) {
						grn = atoi(&pch[1]);
						pch[0] = '\0';		/* Strip the ",num" part */
					}
					if ((pch = strstr(stem,"+")) != NULL) {
						writeLevel = atoi(pch++);
						if (writeLevel < 0) writeLevel = 0;
						pch--;
						pch[0] = '\0';		/* Hide the +lev from the stem string */
						saveNested = TRUE;
					}
					if (argv[i][1] == 'G') 
						write_grids = TRUE;
					else {
						out_3D = TRUE;
						fname3D = stem;
						if (fname3D[strlen(fname3D)-3] != '.' && fname3D[strlen(fname3D)-4] != '.')
							strcat(fname3D, ".nc");		/* If no 2 or 3 letters extension, add .nc */
					}
					break;
				case 'H':	/* Hot start stuff */
					if (!argv[i][2])					/* Output momentum grids */
						out_momentum = TRUE;
					else if (argv[i][2] == 's' && argv[i][3] == ',')	/* NOT YET. Maybe it will be -Hs,time_to_stop */
						;
					else {
						sscanf(&argv[i][2], "%s", str_tmp);
						if ((pch = strstr(str_tmp,",")) != NULL) {
							pch[0] = '\0';
							strcpy(fname_momentM, str_tmp);                         /* File names of moment M & N files */
							strcpy(fname_momentN, ++pch);
							if ((pch = strstr(fname_momentN,",")) != NULL) {        /* We have the hot start time as well */
								pch[0] = '\0';                                      /* The end of the moment N file name */
								time_h += atof(++pch);                              /* Increment the starting time (was zero) */
							}
							do_HotStart = TRUE;
						}
						else {
							mexPrintf("NSWING: Error, -H option (Hot start), must provide names of moment_X, moment_Y files.\n");
							error++;							
						}
					}
					break;
				case 'J':	/* Jumping options. Accept either -Jn, -J+m, -Jn+m or -Jn -J+m */
					sscanf(&argv[i][2], "%s", str_tmp);
					if ((pch = strstr(str_tmp,"+")) != NULL) {
						sscanf((++pch), "%lf", &nest.run_jump_time);
						pch--;
						pch[0] = '\0';		/* Put the string end where before was the '+' char */
					}
					if (argv[i][2])
						sscanf(&argv[i][2], "%lf", &time_jump);

					break;
				case 'L':	/* Use linear approximation or Lagrangean tracers*/
					if (!argv[i][2])
						nest.do_linear = TRUE;
					else {
						sscanf(&argv[i][2], "%s", str_tmp);
						if (str_tmp[strlen(str_tmp)-2] == '+') {	/* Output tracers file will be in netCDF */
							out_oranges_nc = TRUE;
							str_tmp[strlen(str_tmp)-2] = '\0';
						}
						if ((pch = strstr(str_tmp,",")) != NULL) {
							char *pch2;
							pch[0] = '\0';
							strcpy(tracers_infile, str_tmp);
							strcpy(tracers_outfile, ++pch);		/* NEED TO TEST IF WE GOT A FNAME */
						}
						else {
							mexPrintf("NSWING: Error, -L option, must provide at least the tracers file name\n");
							error++;
						}
						do_tracers = TRUE;
						out_velocity_x = out_velocity_y = TRUE;
					}
					break;
				case 'M':	/* Max/min options */
					if (argv[i][2] == '-') {	/* Compute a mask with ones over the "dried beach" */
						nest.do_long_beach = TRUE;
						if (argv[i][3])
							sscanf(&argv[i][3], "%s", fname_mask_lbeach);
						else
							strcpy(fname_mask_lbeach, "long_beach.grd");
					}
					else if (argv[i][2] == '+') {	/* Compute a mask with ones over the "innundated beach" */
						nest.do_short_beach = TRUE;
						if (argv[i][3])
							sscanf(&argv[i][3], "%s", fname_mask_sbeach);
						else
							strcpy(fname_mask_sbeach, "short_beach.grd");
					}
					else
						max_level = TRUE;
					break;
				case 'N':	/* Number of cycles to compute */
					n_of_cycles = atoi(&argv[i][2]);
					break;
				case 'O':	/* Time interval and fname for maregraph data. Use only when mareg locations were sent in as arg (MEX) */
					sscanf(&argv[i][2], "%s", str_tmp);
					if ((pch = strstr(str_tmp,",")) != NULL) {
						pch[0] = '\0';
						cumint = atoi(str_tmp);
						strcpy(hcum, ++pch);
					}
					else { 
						mexPrintf("NSWING: Error, -O option, must provide interval and output maregs file name\n");
						error++;
					}
					break;
				case 'Q':	/* Vertical offset (simulate tide) */
					if (argv[i][2])
						sscanf(&argv[i][2], "%lf", &z_offset);

					break;					
				case 'R':
					error += decode_R(argv[i], &dfXmin, &dfXmax, &dfYmin, &dfYmax);
					got_R = TRUE;
					break;
				case 'S':	/* Output velocity grids */ 
					strcpy(str_tmp, &argv[i][2]);
					if ((pch = strstr(str_tmp,"+m")) != NULL) {    /* Velocity at maregraphs */
						out_maregs_velocity = TRUE;
						out_velocity_x      = out_velocity_y = TRUE;
						for (k = 0; k < 2; k++) {   /* Strip the '+m' from the str_tmp string */
							len = strlen(pch);
							for (j = 0; j < (int)len; j++)
								pch[j] = pch[j+1];
						}
					}
					if ((pch = strstr(str_tmp,"+s")) != NULL) {    /* Comput max speed */
						max_velocity   = TRUE;
						out_velocity_x = out_velocity_y = TRUE;
						for (k = 0; k < 2; k++) {   /* Strip the '+s' from the str_tmp string */
							len = strlen(pch);
							for (j = 0; j < (int)len; j++)
								pch[j] = pch[j+1];
						}
					}
					if (str_tmp[0] == 'x') {         /* Only X component */
						out_velocity   = out_velocity_x = TRUE;
						if (!(out_maregs_velocity || max_velocity)) out_velocity_y = FALSE;
					}
					else if (str_tmp[0] == 'y') {    /* Only Y component */
						out_velocity   = out_velocity_y = TRUE;
						if (!(out_maregs_velocity || max_velocity)) out_velocity_x = FALSE;
					}
					else if (str_tmp[0] == 'r') {    /* Speed (velocity module) -- NOT YET -- */
						out_velocity   = out_velocity_r = TRUE;
						if (!(out_maregs_velocity || max_velocity)) out_velocity_x = out_velocity_y = FALSE;
					}
					else if (str_tmp[0] == 'n') {    /* No grids, only vx,vy in maregs */
						out_velocity_x = out_velocity_y = TRUE;
						out_velocity   = FALSE;
					}
					else {                          /* Both X & Y*/
						out_velocity = out_velocity_x = out_velocity_y = TRUE;
					}
					break;
				case 't':	/* Time step of simulation */ 
					dt = atof(&argv[i][2]);
					nest.dt[0] = dt;
					break;
				case 'T':	/* File with time interval (n steps), maregraph positions and optional output fname */
					if (cumpt) {
						mexPrintf("NSWING: Error, this option is not to be used when maregraphs were transmitted in input\n");
						mexPrintf("        Ignoring it.\n");
						break;
					}
					sscanf(&argv[i][2], "%s", str_tmp);
					if (str_tmp[strlen(str_tmp)-2] == '+') {	/* Output maregs file will be in netCDF */
						out_maregs_nc = TRUE;
						str_tmp[strlen(str_tmp)-2] = '\0';
					}
					if ((pch = strstr(str_tmp,",")) != NULL) {
						char *pch2;
						pch[0] = '\0';
						if ((pch2 = strstr(str_tmp,".")) != NULL)
							mexPrintf("NSWING: WARNING, 'int' in option -T<int> must be an integer number. Expect surprises.\n");

						cumint = atoi(str_tmp);
						if ((pch2 = strstr(++pch,",")) != NULL) {
							pch2[0] = '\0';
							strcpy(maregs, pch);
							strcpy(hcum, ++pch2);
						}
						else
							strcpy(maregs, pch);
					}
					else {
						mexPrintf("NSWING: Error, -T option, must provide at least a interval and maregs file name\n");
						error++;
					}
					cumpt = TRUE;
					maregs_in_input = FALSE;
#ifndef HAVE_NETCDF
					if (out_maregs_nc) {
						mexPrintf("NSWING: Error, -T cannot choose an netCDF output because this exe was not linked to netCDF.\n");
						out_maregs_nc = FALSE;
					}
#endif
					break;
				case 'X':		/* Manning coeffs */
					k = 0;
					sscanf(&argv[i][2], "%s", str_tmp);
					if ((pch = strstr(str_tmp,"+")) != NULL) {
						nest.manning_depth = -atof(++pch);	/* Reverse sense right away because bat will be pos down */
						pch--;	pch[0] = '\0';		/* Remove traces of this option in string */
					}
					if ((pch = strstr(str_tmp,",")) != NULL) {
						char t[16] = "";
						pch[0] = '\0';
						nest.manning[k++] = atof(str_tmp);
						strcpy(t, ++pch);
						while ((pch = strstr(t,",")) != NULL) {
							pch[0] = '\0';
							nest.manning[k++] = atof(t);
							strcpy(t, ++pch);
						}
						nest.manning[k] = atof(t);	/* Last one doesn't end with a comma so was skipped above */
					}
					else
						nest.manning[k] = atof(&argv[i][2]);

					if (k == 0)			/* Only one set. Replicate it to the others (being used or not) */
						for (n = 1; n < 10; n++)
							nest.manning[n] = nest.manning[0];
					break;
				case 'U':
					nest.do_upscale = TRUE;
					break;
				case 'V':
					verbose = TRUE;
					break;
				case '1':
					nesteds[0] = &argv[i][2];
					break;
				case '2':
				case '3':
				case '4':
				case '5':
				case '6':
				case '7':
				case '8':
				case '9':
				case '10':
					nesteds[atoi(&argv[i][1])-1] = &argv[i][2];
					break;
				default:
					mexPrintf("NSWING: Unknown option %s\n", argv[i]);
					error = TRUE;
					break;
			}
		}
		else {
			if (argv[i][0] == ' ' && argv[i][1] == '\0')	/* An empty option (not uncommon from the Matlab side) */
				continue;
			if (bathy == NULL)
				bathy = argv[i];
			else if (fonte == NULL)
				fonte = argv[i];
			else if (bathy != NULL && fonte != NULL) {
				mexPrintf("NSWING: Wrong option %s (misses the minus sign)\n", argv[i]);
				error = TRUE;
			}
		}
	}

	if (argc <= 1 || error) {
#ifdef LIMIT_DISCHARGE
		mexPrintf("NSWING - A tsunami maker (%s)\t\t-- With DISCHARGE limit.\n\n", prog_id);
#else
		mexPrintf("NSWING - A tsunami maker (%s)\n\n", prog_id);
#endif

#ifdef I_AM_MEX
		mexPrintf("nswing(bat,hdr_bat,deform,hdr_deform, [-1<bat_lev1>], [-2<bat_lev2>], [-3<...>] [maregs], [-G|Z<name>[+lev],<int>],\n");
		mexPrintf("       [-A<fname.sww>], [-B<BCfile>], [-C], [-D], [-E[p][m][,decim]], [-Fdip/strike/rake/slip/length/width/topDepth/x_epic/y_epic],\n");
		mexPrintf("       [-Fk[c]<w/e/s/n>], [-H], [-H<momentM,momentN>[,t]], [-J<time_jump>[+run_time_jump]], [-L[name1,name2]],,\n");
		mexPrintf("       [-M[-|+[<maskname>]]], [-N<n_cycles>], [-R<w/e/s/n>], [-S[x|y|n][+m][+s]], [-O<int>,<outmaregs>],\n");
		mexPrintf("       [-Q<z_offset>], [-S[x|y|n][+m][+s]], [-T<int>,<mareg>[,<outmaregs[+n]>]], [-X<manning0[,...]>] -t<dt> [-f]\n");
#else
		mexPrintf("nswing bathy.grd initial.grd [-1<bat_lev1>] [-2<bat_lev2>] [-3<...>] [-G|Z<name>[+lev],<int>] [-A<fname.sww>]\n");
		mexPrintf("       [-B<BCfile>] [-C] [-D] [-E[p][m][,decim]] [-Fdip/strike/rake/slip/length/width/topDepth/x_epic/y_epic]\n"); 
		mexPrintf("       [-Fk[c]<w/e/s/n>] [-H] [-H<momentM,momentN>[,t]] [-J<time_jump>[+run_time_jump]] [-L[name1,name2]]\n");
		mexPrintf("       [-M[-|+[<maskname>]]] [-N<n_cycles>] [-R<w/e/s/n>] [-S[x|y|n][+m][+s]] [-T<int>,<mareg>[,<outmaregs[+n]>]]\n");
		mexPrintf("       [-Q<z_offset>] [-X<manning0[,...]>] -t<dt> [-f]\n");
#endif
		mexPrintf("\t-A <name> save result as a .SWW ANUGA format file\n");
		mexPrintf("\t-n basename for MOST triplet files (no extension)\n");
		mexPrintf("\t-B name of a BoundaryCondition ASCII file\n");
		mexPrintf("\t-C Add Coriolis effect.\n");
		mexPrintf("\t-D write grids with the total water depth. These grids will have wave height on ocean\n");
		mexPrintf("\t   and water thickness on land.\n");
		mexPrintf("\t-E write grids with energy or power (-Ep). Apend a 'm' to save only one grid with the max values.\n");
		mexPrintf("\t   Since this can noticeably slow down the run, one can append a decimator factor after the comma.\n");
		mexPrintf("\t   Note, however, that this casuses aliasing that is clearly visible on shaded illumation.\n");
		mexPrintf("\t   The file name is controled by the <name> in the -G or -Z options, complemented with a '_max' prefix,\n");
		mexPrintf("\t   but the saving of multiple grids is disabled. However, it is still possible to save a 3D netCDF\n");
		mexPrintf("\t   file with wave heights if -Z is used.\n");
		mexPrintf("\t-F dip/strike/rake/slip/length/width/topDepth/x_epic/y_epic\n");
		mexPrintf("\t   Fault parameters describing Dip,Azimuth,Rake,Slip(m),lenght,height and depth from sea-bottom\n");
		mexPrintf("\t   x_epic, y_epic X and Y coordinates of begining of fault trace. All dimensions must be in km.\n");
		mexPrintf("\t-Fk<west/east/south/north> Build a prism source with these limits and height of 1 meter.\n");
		mexPrintf("\t-Fkc<x/y/nx/ny>. Alternatively, provide the prism size as center at x/y and nx/ny half-widths cell number.\n");
		mexPrintf("\t-Fk.../RxC. Loops over a matrix of size R x C satrting at Lower Left Corner given by w/e/s/n.\n");
		mexPrintf("\t-Fk.../dx[/dy]. Given the w/e/s/n region (Pixel registration) loop over the number of prisms\n");
		mexPrintf("\t   obtained by dividing the regin in increments of dx/dy (if not given defaults dy = dx).\n");
		mexPrintf("\t   The use of -Fk sets the output maregraph file to netCDF format, unless rows = cols = 1.\n");
		mexPrintf("\t-G <stem> write grids at the <int> intervals. Append file prefix. Files will be called <stem>#.grd\n");
		mexPrintf("\t   When doing nested grids, append +lev to save that particular level (only one level is allowed)\n");
		mexPrintf("\t-H write grids with the momentum. i.e velocity times water depth.\n");
		mexPrintf("\t-H <fname_momentM,fname_momentN>[,t] Do Hot start using these moment grids. Optional 't' is the\n");
		mexPrintf("\t   time of hot start. (Need also surface displacement corresponding to the time of these grids.)\n");
		mexPrintf("\t-J <time_jump> Do not write grids or maregraphs for times before time_jump in seconds.\n");
		mexPrintf("\t   When doing nested grids, append +<time> to NOT start computations of nested grids before this\n");
		mexPrintf("\t   time has elapsed. Any of these forms is allowed: -Jt1, -J+t2, -Jt1+t2 or -Jt1 -J+t2\n");
		mexPrintf("\t-L Use linear approximation in moment conservation equations (faster but less good).\n");
		mexPrintf("\t-L <in_fname>,<out_fname> Do Lagragian tracers, where <in_fname> is the file name of the tracers\n");
		mexPrintf("\t   initial position and <out_fname> the file name to hold the results.\n");
		mexPrintf("\t-M write a grid with the max water level. The file name is controled by the <name> in the -Z option,\n");
		mexPrintf("\t   complemented with a '_max' prefix.\n");
		mexPrintf("\t   Append a '-' to compute instead the maximum water retreat. The result is writen to a\n");
		mexPrintf("\t   mask file, witch by default is called 'long_beach.grd'. To use a different name append it\n");
		mexPrintf("\t   after the '-' sign. Example: -M-beach_long.grd\n");
		mexPrintf("\t   Append a '+' to compute instead a mask with the Run In extent. Otherwise behaves like -M-.\n");
		mexPrintf("\t   You can repeat -M to compute any of the above. I.e. -M -M- -M+ will compute all three..\n");
		mexPrintf("\t   Note that if -Z was used the 'long' and 'short' beach arrays will be saved in the .nc file too.\n");
		mexPrintf("\t-N number of cycles [Default 1010].\n");
#ifdef I_AM_MEX
		mexPrintf("\t-O <int>,<outfname> interval at which maregraphs are writen to the <outfname> maregraph file.\n");
#endif
		mexPrintf("\t-Q <z_offset> Apply a vertical offset to ALL bathymetry grids. Use it to simulate tide.\n");
		mexPrintf("\t-R output grids only in the sub-region enclosed by <west/east/south/north>\n");
		mexPrintf("\t-S write grids with the velocity. Grid names are appended with _U and _V sufixes.\n");
		mexPrintf("\t   Use x or y to save only one of those components. But use n to not velocity grids (maregs only).\n");
		mexPrintf("\t   Append +m to write also velocity (vx,vy) at maregraphs locations (needs -T and/or -O).\n");
		mexPrintf("\t   Append +s to write the max speed (|v|). Grid name is appended with _max_speed suffix.\n");
		mexPrintf("\t   Use also the the 'n' flag to NOT output the U and V components. e.g -Sn+s\n");
		mexPrintf("\t-T <int> interval at which maregraphs are writen to the output maregraph file.\n");
		mexPrintf("\t   <maregs> file name with the (x y) location of the virtual maregraphs.\n");
		mexPrintf("\t   <outmaregs> optional file name where to save the maregraphs output.\n");
		mexPrintf("\t   If not provided the output name will be constructed by appending '_auto.dat' to <maregs>.\n");
		mexPrintf("\t   In any case append a +n to choose writing the maregraphs as a netCDF file.\n");
#ifdef I_AM_MEX
		mexPrintf("\t   Warning: this option cannot be used when maregraphs were transmitted in input.\n");
#endif
		mexPrintf("\t-X <maning0[,maning1[,...]][+<depth>]> Manning friction coefficients. If only one provided, use it for all\n");
		mexPrintf("\t   nesting levels (if applyable), otherwise specify one for each nesting level separated by commas.\n");
		mexPrintf("\t   Append +<depth> to only apply Manning at depths shallower than depth (pos up).\n");
		mexPrintf("\t-Z Same as -G but saves result in a 3D netCDF file.\n");
		mexPrintf("\t-t <dt> Time step for simulation.\n");
		mexPrintf("\t-f To use when grids are in geographical coordinates.\n");
#ifdef I_AM_MEX
		mexPrintf("\t-e To be used from the Mirone stand-alone version.\n");
		return;
#else
		return error;
#endif
	}

	do_maxs = (max_level || max_energy || max_power);
	do_2Dgrids = (write_grids || out_velocity || out_velocity_x || out_velocity_y || out_velocity_r || out_momentum
	              || max_level || max_velocity || max_energy || out_power || max_power || nest.do_long_beach
	              || nest.do_short_beach);

	if (!(do_2Dgrids || out_sww || out_most || out_3D || cumpt)) {
		mexPrintf("Nothing selected for output (grids, or maregraphs), exiting\n");
		error++;
	}

	if (grn == 0 && !do_maxs && !cumpt) {
		mexPrintf("NSWING: Error, -G or -Z option. MUST provide saving interval\n");
		error++;
	}
	if (!stem && !cumpt) {
		mexPrintf("NSWING: Error, -G or -Z option. MUST provide base name || OR -T option\n");
		error++;
	}

	if (water_depth && (out_sww || out_most)) {
		water_depth = FALSE;
		mexPrintf("WARNING: Total water option is not compatible with ANUGA|MOST outputs. Ignoring\n");
	}

	if (do_Kaba && fonte)
		mexPrintf("WARNING: Source file is ignored when -Fk option is used.\n");

	if (dt <= 0) {
		mexPrintf("NSWING: Error -t option. Time step of simulation not provided or negative.\n");
		error++;
	}

	if (out_sww && fname_sww == NULL) {
		mexPrintf("NSWING: Error -A option. Must provide a name for the .SWW file.\n");
		error++;
	}

	if (out_momentum && (out_velocity_x || out_velocity_y)) {
		mexPrintf("NSWING: Error -S / -H options. Can only select one off velocity OR momentum output.\n");
		error++;
	}

	if (nest.do_Coriolis && !isGeog && nest.lat_min4Coriolis == -100) {
		mexPrintf("NSWING: Error -C option. For cartesian grids must provide the South latitude. Ignoring Corilis request.\n");
		nest.do_Coriolis = FALSE;
	}

	if (cumpt) {		/* Deal with the several aspects of reading a maregraphs file */
		if (cumint <= 0) {
			mexPrintf("NSWING: error, -T or -O options imply a saving interval\n");
			Return(-1);
		}
		else if (!maregs) {
			mexPrintf("NSWING: error, -T or -O options imply a maregs file\n");
			Return(-1);
		}
		else if (!hcum || !strcmp(hcum, "")) {
			len = strlen(maregs) - 1;
			while (maregs[len] != '.') len--;
			if (len <= 0)
				strcat(strcpy(hcum, maregs), (out_maregs_nc) ? "_auto.nc" : "_auto.dat");
			else {
				strcpy(hcum, maregs);
				hcum[len] = '\0';
				strcat(hcum, (out_maregs_nc) ? "_auto.nc" : "_auto.dat");
			}
		}

		n_ptmar = n_of_cycles / cumint + 1;
		if (!error && (fp = fopen (hcum, "w")) == NULL) {
			mexPrintf("%s: Unable to create file %s - exiting\n", "nswing", hcum);
			Return(-1);
		}
		if (!error && !maregs_in_input) {
			n_mareg = count_n_maregs(maregs);          /* Count maragraphs number */
			if (n_mareg < 0) {
				Return(-1);            /* Warning already issued in count_n_maregs() */
			}
			else if (n_mareg == 0) {
				mexPrintf ("NSWING: Warning file %s has no valid data.\n", maregs);
				cumpt = FALSE;
			}
		}
	}

	if (do_tracers) {	/* Count number of oranges */
		n_oranges = count_n_maregs(tracers_infile);    /* Count tracers number */
		if (n_oranges <= 0) {
			mexPrintf("NSWING: Warning file %s has no valid data. Ignoring this option\n", tracers_infile);
			do_tracers = FALSE;			
		}
	}

	if (out_momentum && (out_sww || out_most)) out_momentum = FALSE;
	if ((out_velocity || out_velocity_x || out_velocity_y || out_velocity_r) && (out_sww || out_most)) out_velocity = FALSE;

	if (!bat_in_input && !source_in_input) {			/* If bathymetry & source where not given as arguments, load them */
		if (!bathy || (!fonte && !bnc_file && !do_Okada && !do_Kaba)) {
			mexPrintf("NSWING: error, bathymetry and/or source grids were not provided.\n"); 
			Return(-1);
		}

		r_bin_b = read_grd_info_ascii(bathy, &hdr_b);	/* To know how what memory to allocate */
		if (r_bin_b < 0) {
			mexPrintf("NSWING: %s Invalid bathymetry grid. Possibly it is in the Surfer 7 format\n", bathy); 
			Return(-1);
		}
		
		if (!do_Okada && !do_Kaba) {	/* Otherwise we will compute initial condition later down after arrays are allocated */
			if (!bnc_file) r_bin_f = read_grd_info_ascii(fonte, &hdr_f);	/* and check that both grids are compatible */
			if (r_bin_f < 0) {
				mexPrintf("NSWING: %s Invalid source grid. Possibly it is in the Surfer 7 format\n", fonte); 
				Return(-1);
			}

			if (!bnc_file && (hdr_f.nx != hdr_b.nx || hdr_f.ny != hdr_b.ny)) {
				mexPrintf("Bathymetry and source grids have different rows/columns\n");
				mexPrintf("%d %d %d %d\n", hdr_b.ny, hdr_f.ny, hdr_b.nx, hdr_f.nx);
				error++;
			}
		}
	}

	if (do_HotStart) {		/* Check that the moment grids for Hot Start are compatible */
		r_bin_mM = read_grd_info_ascii(fname_momentM, &hdr_mM);
		r_bin_mN = read_grd_info_ascii(fname_momentN, &hdr_mN);
		if (hdr_b.nx != hdr_mM.nx || hdr_b.ny != hdr_mM.ny || hdr_b.nx != hdr_mN.nx || hdr_b.ny != hdr_mN.ny) {
			mexPrintf("Bathymetry and moment grids have different rows/columns\n");
			error++;
		}
	}

	dx = (hdr_b.x_max - hdr_b.x_min) / (hdr_b.nx - 1);
	dy = (hdr_b.y_max - hdr_b.y_min) / (hdr_b.ny - 1);
	if (!bnc_file && !do_Okada && !do_Kaba) {
		if (fabs(hdr_f.x_min - hdr_b.x_min) / dx > dx / 4 || fabs(hdr_f.x_max - hdr_b.x_max) / dx > dx / 4 ||
			fabs(hdr_f.y_min - hdr_b.y_min) / dy > dy / 4 || fabs(hdr_f.y_max - hdr_b.y_max) / dy > dy / 4 ) {
			mexPrintf("Bathymetry and source grids do not cover the same region\n"); 
			mexPrintf("%lf %lf %lf %lf\n", hdr_f.x_min, hdr_b.x_min, hdr_f.x_max, hdr_b.x_max); 
			mexPrintf("%lf %lf %lf %lf\n", hdr_f.y_min, hdr_b.y_min, hdr_f.y_max, hdr_b.y_max); 
			error++;
		}
	}

	/* ------------------------- Check the CFL condition ----------------------------------- */
	ds = MIN(dx, dy);
	if (isGeog) ds *= 111000;		/* Get it in metters */
	dtCFL = ds / sqrt(fabs(hdr_b.z_min) * 9.8);
	if (dt > dtCFL) {
		mexPrintf("NSWING: Error: dt is greater than dtCFL (%.3f). No way that this would work. Stopping here.\n", dtCFL); 
		Return(-1);
	}
	else if (dt > (dtCFL / 2)*1.1)		/* With a margin of 10% before triggering the warning */
		mexPrintf("NSWING: Warning: dt > dtCFL / 2 is normaly not good enough. "
		                   "This may cause troubles. Consider using ~ %.3f\n", dtCFL/2); 

	if (error) Return(-1);

	if (n_arg_no_char == 0) {		/* Read the nesting grids (when we have them ofc) */
		int r_bin;
		double dx, dy;		/* Local variables to not interfere with the base level ones */
		struct	srf_header hdr;

		num_of_nestGrids = 0;
		while (nesteds[num_of_nestGrids] != NULL) {
			if ((r_bin = read_grd_info_ascii(nesteds[num_of_nestGrids], &hdr)) < 0) {
				mexPrintf("NSWING: %s Invalid bathymetry grid. Possibly it is in the Surfer 7 format\n",
					nesteds[num_of_nestGrids]); 
				Return(-1);
			}
			if ((nest.bat[num_of_nestGrids+1] = (double *)mxCalloc((size_t)hdr.nx*(size_t)hdr.ny, sizeof(double)) ) == NULL) 
				{no_sys_mem("(bat)", hdr.nx*hdr.ny); Return(-1);}

			if (!r_bin) {
				if (read_grd_ascii(nesteds[num_of_nestGrids], &hdr, nest.bat[num_of_nestGrids+1], -1))
					Return(-1);
			}
			else {
				if (read_grd_bin(nesteds[num_of_nestGrids], &hdr, nest.bat[num_of_nestGrids+1], -1))
					Return(-1);
			}

			dx = (hdr.x_max - hdr.x_min) / (hdr.nx - 1);
			dy = (hdr.y_max - hdr.y_min) / (hdr.ny - 1);
			nest.hdr[num_of_nestGrids+1].nx = hdr.nx;
			nest.hdr[num_of_nestGrids+1].ny = hdr.ny;
			nest.hdr[num_of_nestGrids+1].nm = (unsigned int)hdr.nx * (unsigned int)hdr.ny;
			nest.hdr[num_of_nestGrids+1].x_inc = dx;           nest.hdr[num_of_nestGrids+1].y_inc = dy;
			nest.hdr[num_of_nestGrids+1].x_min = hdr.x_min;    nest.hdr[num_of_nestGrids+1].x_max = hdr.x_max;
			nest.hdr[num_of_nestGrids+1].y_min = hdr.y_min;    nest.hdr[num_of_nestGrids+1].y_max = hdr.y_max;
			nest.hdr[num_of_nestGrids+1].z_min = hdr.z_min;    nest.hdr[num_of_nestGrids+1].z_max = hdr.z_max;
			num_of_nestGrids++;
		}
		do_nestum = (num_of_nestGrids) ? TRUE : FALSE;
	}

	/* Check if nesting grids fit nicely within each others */
	if (do_nestum && check_paternity(&nest)) Return(-1);

	if (writeLevel > num_of_nestGrids) {
		mexPrintf("Requested save grid level is higher that actual number of nested grids. Using last\n");
		writeLevel = num_of_nestGrids;
		if (writeLevel < 0) writeLevel = 0;     /* When num_of_nestGrids is zero */
	}

	if (cumpt && !writeLevel && do_nestum)      /* The case of maregraphs ONLY and nested grids. Use LAST level */
		writeLevel = num_of_nestGrids;

	/* -------------------------------------------------------------------------------------- */
	for (k = 0; k < argc; k++) {		/* Build the History string (incomplete if AM_MEX) */
		strcat(history, argv[k]);		strcat(history, " ");
	}
	/* -------------- Allocate memory and initialize the 'nest' structure ------------------- */
	nest.hdr[0].nx      = hdr_b.nx;		nest.hdr[0].ny = hdr_b.ny;
	nest.hdr[0].nm      = (unsigned int)hdr_b.nx * (unsigned int)hdr_b.ny;
	nest.out_velocity_x = out_velocity_x;
	nest.out_velocity_y = out_velocity_y;
	nest.out_momentum   = out_momentum;
	nest.isGeog = isGeog;
	nest.writeLevel = writeLevel;
	if (initialize_nestum(&nest, isGeog, 0)) Return(-1);

	/* We need the ''work' array in most cases, but not all and also need to make sure it's big enough */
	if ((out_most || out_3D || surf_level || water_depth || out_energy || out_power || out_momentum ||
		out_velocity || out_velocity_x || out_velocity_y || out_velocity_r || do_maxs || surf_level || water_depth) &&
		(work = (float *) mxCalloc((size_t)(nest.hdr[0].nm, nest.hdr[writeLevel].nm), sizeof(float)) ) == NULL)
			{no_sys_mem("(work)", nest.hdr[writeLevel].nm); Return(-1);}

	if ((do_maxs || (nest.long_beach || nest.short_beach)) && 
		(wmax = (float *) mxCalloc((size_t)nest.hdr[writeLevel].nm, sizeof(float)) ) == NULL)
		{no_sys_mem("(wmax)", nest.hdr[writeLevel].nm); Return(-1);}
	if (max_energy || max_power && (workMax = (float *)mxCalloc((size_t)nest.hdr[writeLevel].nm, sizeof(float)) ) == NULL)
		{no_sys_mem("(workMax)", nest.hdr[writeLevel].nm); Return(-1);}
	/* Copy these pointers to use in update_max() */
	nest.work = work;
	nest.wmax = wmax;
	if (max_velocity && (vmax = (float *)mxCalloc((size_t)nest.hdr[writeLevel].nm, sizeof(float)) ) == NULL)
		{no_sys_mem("(vmax)", nest.hdr[writeLevel].nm); Return(-1);}
	nest.vmax = vmax;
	/* -------------------------------------------------------------------------------------- */

	if (bat_in_input) {		/* If bathymetry & source where given as arguments */
		/* Transpose from Matlab orientation to scanline orientation */
		for (i = 0; i < hdr_b.ny; i++)
			for (j = 0; j < hdr_b.nx; j++)
				nest.bat[0][i*hdr_b.nx+j] = -dep1[j*hdr_b.ny+i];

		for (i = 0; i < hdr_f.ny; i++) {
			for (j = 0; j < hdr_f.nx; j++)
				nest.etaa[0][i*hdr_f.nx+j] = h[j*hdr_f.ny+i];
		}
	}
	else {			/* If bathymetry & source where not given as arguments, load them */
		if (!r_bin_b)					/* Read bathymetry */
			read_grd_ascii(bathy, &hdr_b, nest.bat[0], -1);
		else
			read_grd_bin(bathy, &hdr_b, nest.bat[0], -1);

		if (bnc_file == NULL) {
			if (do_Okada)				/* compute the initial condition */
				deform(hdr_b, dx, dy, isGeog, f_length, f_width, f_azim, f_dip, f_rake, f_slip,
				        f_topDepth, x_epic, y_epic, nest.etaa[0]);
			else if (do_Kaba) {
				kaba_source(hdr_b, dx, dy, kaba_xmin, kaba_xmax, kaba_ymin, kaba_ymax, do_Kaba, nest.etaa[0]);
			}
			else {
				r_bin_b = read_grd_info_ascii(fonte, &hdr_f);	/* To know if bin or asc (but idiot, fun should do it all) */
				if (r_bin_b)			/* Read source */
					read_grd_bin(fonte, &hdr_f, nest.etaa[0], 1);
				else
					read_grd_ascii(fonte, &hdr_f, nest.etaa[0], 1);
			}
		}
	}

	if (do_HotStart) {
		if (!r_bin_mM)					/* Read moment M */
			read_grd_ascii(fname_momentM, &hdr_mM, nest.fluxm_a[0], 1);
		else
			read_grd_bin(fname_momentM, &hdr_mM, nest.fluxm_a[0], 1);

		if (!r_bin_mN)					/* Read moment N */
			read_grd_ascii(fname_momentN, &hdr_mN, nest.fluxn_a[0], 1);
		else
			read_grd_bin(fname_momentN, &hdr_mN, nest.fluxn_a[0], 1);
	}

	hdr.nx = hdr_b.nx;          hdr.ny = hdr_b.ny;
	hdr.nm = (unsigned int)hdr.nx * (unsigned int)hdr.ny;
	hdr.x_inc = dx;             hdr.y_inc = dy;
	hdr.x_min = hdr_b.x_min;    hdr.x_max = hdr_b.x_max;
	hdr.y_min = hdr_b.y_min;    hdr.y_max = hdr_b.y_max;
	hdr.z_min = hdr_b.z_min;    hdr.z_max = hdr_b.z_max;
	hdr.lat_min4Coriolis = 0;	/* PRECISA ACABAR ISTO */
	hdr.doCoriolis = FALSE;

	nest.hdr[0] = hdr;

	if (cumpt && !maregs_in_input) {
		lcum_p = (unsigned int *)mxCalloc((size_t)(1024), sizeof(unsigned int));	/* We wont ever use these many */
		mareg_names = mxCalloc((size_t)(1024), sizeof(char *));
		if ((n_mareg = read_maregs(nest.hdr[writeLevel], maregs, lcum_p, mareg_names)) < 1) {	/* Read maregraph locations */
			mexPrintf("NSWING - WARNING: No maregraphs inside the (inner?) grid\n");
			n_mareg = 0;
			if (lcum_p) mxFree(lcum_p);
			mxFree((void *) cum_p);	mxFree((void *) time_p);	 
			cumpt = FALSE;
		}
	}

	/* ------- If we have a tracers (oranges) file, time to load it ------------ */
	if (do_tracers) {
		if ((fp_oranges = fopen(tracers_outfile, "wt")) == NULL) {
			mexPrintf("NSWING: Unable to open output tracers file %s - ignoring this option\n", tracers_outfile);
			do_tracers = FALSE;
		}
		else {
			oranges = (struct tracers *)mxCalloc((size_t)n_oranges, sizeof(struct tracers));
			for (n = 0; n < n_oranges; n++) {
				oranges[n].x = (double *)mxCalloc((size_t)(n_of_cycles), sizeof(double));
				oranges[n].y = (double *)mxCalloc((size_t)(n_of_cycles), sizeof(double));
			}
			if ((n_oranges = read_tracers(nest.hdr[writeLevel], tracers_infile, oranges)) < 1) {	/* Read orange locations */
				mexPrintf("NSWING - WARNING: No tracers inside the (inner?) grid\n");
				for (n = 0; n < n_oranges; n++) {mxFree(oranges[n].x);		mxFree(oranges[n].y);}
				do_tracers = FALSE;
			}
			/* Select which vx/vy will be used to compute the lagragian tracers */
			vx_for_oranges     = nest.vex[writeLevel];
			vy_for_oranges     = nest.vey[writeLevel];
			fluxm_for_oranges  = nest.fluxm_d[writeLevel];
			fluxn_for_oranges  = nest.fluxn_d[writeLevel];
			htotal_for_oranges = nest.htotal_d[writeLevel];
		}
	}

	/* ------- If we have a boundary condition file, time to load it ------------ */
	if (bnc_file) {
		int side_len;

		if (read_bnc_file(&nest, bnc_file)) Return(-1);
		wall_it(&nest);       /* Set Wall boundary conditions */

		/* Allocate and initialize vectors for boundary conditions*/
		if ((nest.bnc_border[0] != 0) || (nest.bnc_border[2] != 0))	/* West or East borders */ 
			side_len = nest.hdr[0].ny;
		else
			side_len = nest.hdr[0].nx;

		nest.bnc_var_z_interp = (double *)mxMalloc((size_t)side_len * sizeof(double));

		if (nest.bnc_pos_nPts > 1) {	/* In this (not yet) case we need to deal with these guys */
			nest.edge_row_P[0] = (double *) mxCalloc((size_t)side_len, sizeof(double));
			if (side_len == nest.hdr[0].nx) {
				for (i = 0; i < nest.hdr[0].nx; i++)
					nest.edge_row_P[0][i] = nest.hdr[0].x_min + i * nest.hdr[0].x_inc;   /* XX coords along S/N edge */
			}
			else {
				for (i = 0; i < nest.hdr[0].ny; i++)
					nest.edge_row_P[0][i] = nest.hdr[0].y_min + i * nest.hdr[0].y_inc;   /* YY coords along S/N edge */
			}
		}
	}

#ifdef I_AM_MEX
	if (!IamCompiled) {
		rhs[0] = mxCreateDoubleScalar(0.0);
		ptr_wb = mxGetPr(rhs[0]);
		rhs[1] = mxCreateString("title");
		rhs[2] = mxCreateString("Sit and Wait ...");
		mexCallMATLAB(0,NULL,3,rhs,"aguentabar");
	}
	else
		mexEvalString("aguentabar(0,\"title\",\"Sit and Wait...\")");
#endif

	/* ----------------- Compute vars to use if write grids --------------------- */
	if (!got_R && (do_2Dgrids || out_sww || out_most || out_3D) ) {	
		/* Write grids over the whole region */
		i_start = 0;            i_end = nest.hdr[writeLevel].nx;
		j_start = 0;            j_end = nest.hdr[writeLevel].ny;
		xMinOut = nest.hdr[writeLevel].x_min;	yMinOut = nest.hdr[writeLevel].y_min;
	}
	else if (got_R && (do_2Dgrids || out_sww || out_most || out_3D)) {	
		/* Write grids in sub-region */
		i_start = irint((dfXmin - nest.hdr[writeLevel].x_min) / nest.hdr[writeLevel].x_inc);
		j_start = irint((dfYmin - nest.hdr[writeLevel].y_min) / nest.hdr[writeLevel].y_inc); 
		i_end   = irint((dfXmax - nest.hdr[writeLevel].x_min) / nest.hdr[writeLevel].x_inc) + 1;
		j_end   = irint((dfYmax - nest.hdr[writeLevel].y_min) / nest.hdr[writeLevel].y_inc) + 1;
		/* Adjustes xMin|yMin to lay on the closest grid node */
		xMinOut = nest.hdr[writeLevel].x_min + nest.hdr[writeLevel].x_inc * i_start;
		yMinOut = nest.hdr[writeLevel].y_min + nest.hdr[writeLevel].y_inc * j_start;
	}
	/* -------------------------------------------------------------------------- */

	if (do_nestum) {                /* Initialize the nest struct array */
		for (k = 1; k <= num_of_nestGrids; k++) {
			if (initialize_nestum(&nest, isGeog, k))
				Return(-1);
		}
		nest.time_h = time_h;
		/* TEMP trick to NOT do initial interpolation of nested grids. For TESTING purposes only. */
		if (nest.run_jump_time > 0 && nest.run_jump_time < nest.dt[0])
			nest.run_jump_time = 0;		/* So that we don't trigger resamplegrid in nestify() */
		else
			resamplegrid(&nest, num_of_nestGrids);	/* Resample eta(s) in descendent grids to avoid initial jumps at borders */
	}

#ifdef HAVE_NETCDF
	if (out_sww) {
		/* ----------------- Open a ANUGA netCDF file for writing --------------- */
		nx = i_end - i_start;		ny = j_end - j_start;
		ncid = open_anuga_sww(&nest, fname_sww, history, ids, i_start, j_start, i_end, j_end, xMinOut, yMinOut, writeLevel);
		if (ncid == -1) {
			mexPrintf("NSWING: failure to create ANUGA SWW file.\n");
			Return(-1);
		}

		/* To be used when writing the data slices */
		count1_A[0] = 1;	count1_A[1] = (i_end - i_start) * (j_end - j_start);

		stage_range[0] = xmom_range[0] = ymom_range[0] = FLT_MAX;
		stage_range[1] = xmom_range[1] = ymom_range[1] = -FLT_MIN;

		tmp_slice = (float *)mxMalloc(sizeof(float) * (nx * ny));       /* To use inside slice writing */ 
	}

	if (out_most) {
		/* ----------------- Open a 3 MOST netCDF files for writing ------------- */
		nx = i_end - i_start;		ny = j_end - j_start;
		ncid_most[0] = open_most_nc(&nest, work, basename_most, "HA", history, ids_ha, nx, ny, xMinOut, yMinOut, TRUE, writeLevel);
		ncid_most[1] = open_most_nc(&nest, work, basename_most, "UA", history, ids_ua, nx, ny, xMinOut, yMinOut, TRUE, writeLevel);
		ncid_most[2] = open_most_nc(&nest, work, basename_most, "VA", history, ids_va, nx, ny, xMinOut, yMinOut, TRUE, writeLevel);

		if (ncid_most[0] == -1 || ncid_most[1] == -1 || ncid_most[2] == -1) {
			mexPrintf ("NSWING: failure to create one or more of the MOST files\n");
			Return(-1);
		}

		ids_most[0] = ids_ha[5];    /* IDs of the Amp, Xmom & Ymom vriables */
		ids_most[1] = ids_ua[5];
		ids_most[2] = ids_va[5];

		/* To be used when writing the data slices */
		count1_M[0] = 1;	count1_M[1] = ny;	count1_M[2] = nx;

		if (!out_sww)               /* Otherwise already allocated */
			tmp_slice = (float *)mxMalloc(sizeof(float) * (nx * ny));   /* To use inside slice writing */ 
	} 
	else if (out_3D) {
		nx = nest.hdr[writeLevel].nx;		ny = nest.hdr[writeLevel].ny;
		ncid_3D[0] = open_most_nc(&nest, work, fname3D, "z", history, ids_z, nx, ny, xMinOut, yMinOut, FALSE, writeLevel);

		if (ncid_3D[0] == -1) {
			mexPrintf ("NSWING: failure to create netCDF file\n");
			Return(-1);
		}
		ids_3D[0] = ids_z[3];       /* ID of z vriable */
		ids_3D[1] = ids_z[5];       /* ID of Vx vriable (only used when it exists) */
		ids_3D[2] = ids_z[6];       /* ID of Vy vriable (only used when it exists) */

		/* To be used when writing the data slices */
		count1_M[0] = 1;	count1_M[1] = ny;	count1_M[2] = nx;
	}
	if (do_Kaba) {
		/* To be used when writing the data slices */
		count_Mar[0] = 1;		/* count_Mar[1] will be set at the end of main loop. */
	}
#endif


	if (do_nestum && saveNested) {
		xMinOut = nest.hdr[writeLevel].x_min;
		yMinOut = nest.hdr[writeLevel].y_min;
		dx = nest.hdr[writeLevel].x_inc;
		dy = nest.hdr[writeLevel].y_inc;
		i_start = 0; j_start = 0;
		i_end = nest.hdr[writeLevel].nx;
		j_end = nest.hdr[writeLevel].ny;
	}

	if (cumpt) {               /* Select which etad/vx/vy will be used to output maregrapghs */
		eta_for_maregs    = nest.etad[writeLevel];
		vx_for_maregs     = nest.vex[writeLevel];
		vy_for_maregs     = nest.vey[writeLevel];
		fluxm_for_maregs  = nest.fluxm_d[writeLevel];
		fluxn_for_maregs  = nest.fluxn_d[writeLevel];
		htotal_for_maregs = nest.htotal_d[writeLevel];

		if (out_maregs_nc) {    /* Allocate an array to hold the maregraph data which will be written to a nc file at the end */
			if ((maregs_array = (float *) mxCalloc((size_t)(n_ptmar * n_mareg), sizeof(float))) == NULL)
				{no_sys_mem("(maregs_array)", n_ptmar * n_mareg); Return(-1);}
			if ((maregs_array_t = (float *) mxCalloc((size_t)(n_ptmar * n_mareg), sizeof(float))) == NULL)	/* A working copy */
				{no_sys_mem("(maregs_array_t)", n_ptmar * n_mareg); Return(-1);}
			if ((maregs_timeout = (double *)mxCalloc((size_t)n_ptmar, sizeof(double))) == NULL)
				{no_sys_mem("(maregs_timeout)", n_ptmar); Return(-1);}
		}
	}

	if (z_offset != 0 && !do_HotStart) {				/* If we have a tide offset, apply it. */
		for (k = 0; k == num_of_nestGrids; k++) {
			for (ij = 0; ij < nest.hdr[k].nm; ij++)
				nest.bat[k][ij] += -z_offset;			/* -z_offset because here bathy is already positive down */
		}
	}

	/* --------------------------------------------------------------------------------------- */
	if (verbose) {
		mexPrintf("\nNSWING: %s\n\n", prog_id);
		mexPrintf("Layer 0  time step = %g\tx_min = %g\tx_max = %g\ty_min = %g\ty_max = %g\n",
		          dt, hdr_b.x_min, hdr_b.x_max, hdr_b.y_min, hdr_b.y_max);
		if (do_nestum) {
			for (k = 1; k <= num_of_nestGrids; k++) {
				mexPrintf("Layer %d x_min = %g\tx_max = %g\ty_min = %g\ty_max = %g\n",
				k, nest.LLx[k], nest.LRx[k], nest.LLy[k], nest.URy[k]);
				mexPrintf("Layer %d inserting index (one based) LL: (row,col) = %d\t%d\t\tUR: (row,col) = %d\t%d\n",
				k, nest.LLrow[k]+2, nest.LLcol[k]+2, nest.URrow[k], nest.URcol[k]);
				mexPrintf("\tTime step ratio to parent grid = %d\n", (int)(nest.dt[k-1] / nest.dt[k]));
				if (k > 1)
					mexPrintf("\t\tdt(parent) = %g\tdt(doughter) = %g\n", nest.dt[k-1], nest.dt[k]);
			}
		}
		mexPrintf ("dtCFL = %.4f\tCourant number (sqrt(g*h)*dt / max(dx,dy)) = %g\n", dtCFL, 1/dtCFL * dt);
		if (nest.do_long_beach) mexPrintf("Output the 'Dry beach' mask.\n");
		if (nest.do_short_beach) mexPrintf("Output the 'Innundated beach' mask.\n");
		if (water_depth)    mexPrintf("Output wave height plus water thickness on land.\n");
		if (out_momentum)   mexPrintf("Output momentum (V * D).\n");
		if (time_jump)      mexPrintf("Hold on %.3f seconds before starting to save results.\n", time_jump);
		if (nest.run_jump_time)
			mexPrintf("Holding on %.3f seconds before start running the nested grids.\n", nest.run_jump_time);
		if (do_maxs) {
			if (max_energy)
				mexPrintf("Output maximum Energy with a decimation of %d\n", decimate_max);
			if (max_power)
				mexPrintf("Output maximum Power with a decimation of %d\n", decimate_max);
		}
		if (nest.do_linear)
			mexPrintf("Using Linear approximation\n");
		if (do_tracers)
			mexPrintf("Computing tracers from file %s \n", tracers_infile);
		if (do_Kaba)
			mexPrintf("Computing a grid of prisms with size %d (rows) x %d (cols)\n", KbGridRows, KbGridCols);
		if (EPS4 != EPS4_)
			mexPrintf("Using a modified EPS4 const of %g\n", EPS4);
#ifdef LIMIT_DISCHARGE
		mexPrintf("\nUsing DISCHARGE limit to minimize sources of instability\n");
#endif
		mexPrintf("\n");
	}
	/* --------------------------------------------------------------------------------------- */

	/* case spherical coordinates initializes parameters */
	if (isGeog == 1) inisp(&nest);
	else if (nest.do_Coriolis) inicart(&nest);

	if (max_level && writeLevel > 0) {    /* Compute the max level of nested grids inside nestify() */
		nest.do_max_level = TRUE;
		max_level = FALSE;                /* Prevent the equivalent code in main loop to be executed */ 
	}

	if (max_velocity && writeLevel > 0) { /* Compute the max velocity of nested grids inside nestify() */
		nest.do_max_velocity = TRUE;
		max_velocity = FALSE;             /* Prevent the equivalent code in main loop to be executed */ 
	}

	tic = clock();

	/* --------------------------------------------------------------------------------------- */
	if (time_jump == 0) time_jump = -1; /* Trick to allow writing zero time grids when jump was not demanded */

	one_100 = (double)(n_of_cycles) / 100.0;

LoopKabas:		/* When computing a grid of Kabas we use a GOTO to simulate a loop. Sorry but have to. */
	/* --------------------------------------------------------------------------------------- */
	/* Begin main iteration */
	/* --------------------------------------------------------------------------------------- */
	for (k = iprc = 0; k < n_of_cycles; k++) {

		if (k > iprc * one_100) {		/* Waitbars stuff */ 
			prc = (double)iprc / 100.;
			iprc++;
			prc = (double)(k+1) / (double)n_of_cycles;
#ifdef I_AM_MEX
			if (!IamCompiled) {
				ptr_wb[0] = prc;
				mexCallMATLAB(0,NULL,1,rhs,"aguentabar");
			}
			else {
				sprintf(cmd, "aguentabar(%f)",prc);
				mexEvalString(cmd);
			}
#else
			fprintf(stderr, "\t%d %%\r", iprc);
#endif
		}

		/* ------------------------------------------------------------------------------------ */
		/* mass conservation */
		/* ------------------------------------------------------------------------------------ */
		if (isGeog == 0)
			mass(&nest, 0);
		else
			mass_sp(&nest, 0);

		/* ------------------------------------------------------------------------------------ */
		/* Case of open boundary condition or wave maker */
		/* ------------------------------------------------------------------------------------ */
		if (bnc_file) {
			/* When the next IF is TRUE it means the bnc file ended to be consumed, so following
			   iterations will use the OPENB() function */
			if (interp_bnc(&nest, time_h)) bnc_file = NULL;
			wave_maker(&nest);   /* Boundary condition was already set (after reading bnc_file) */
		}
		else if (k)
			openb(nest.hdr[0], nest.bat[0], nest.fluxm_d[0], nest.fluxn_d[0], nest.etad[0], &nest);

		/* ------------------------------------------------------------------------------------ */
		/* If Nested grids we have to do the nesting work */
		/* ------------------------------------------------------------------------------------ */
		if (do_nestum) nestify(&nest, num_of_nestGrids, 1, isGeog);

		/* ------------------------------------------------------------------------------------ */
		/* momentum conservation */
		/* ------------------------------------------------------------------------------------ */
		moment_conservation(&nest, isGeog, 0);

		/* ------------------------------------------------------------------------------------ */
		/* update eta and fluxes */
		/* ------------------------------------------------------------------------------------ */
		update(&nest, 0);

		/* ------------------------------------------------------------------------------------ */
		/* If want time series at maregraph positions */
		/* ------------------------------------------------------------------------------------ */
		if (cumpt && (k % cumint == 0)) {
			if (out_maregs_nc) {
				maregs_timeout[count_time_maregs_timeout++] = time_h + dt/2;
				for (ij = 0; ij < n_mareg; ij++)
					maregs_array[count_maregs_timeout++] = (float)eta_for_maregs[lcum_p[ij]];
			}
			else {
				if (k == 0) {		/* Write also the maregraphs coordinates (at grid nodes) */
					int ix, iy, n;
					char *txt[4], t0[16], t1[16], t2[16], t3[16], *txt_X, *txt_Y, fmt[8];	/* for the headers */
					txt[0] = mxCalloc((size_t)n_mareg, 16); txt[1] = mxCalloc((size_t)n_mareg, 16);
					txt[2] = mxCalloc((size_t)n_mareg, 16); txt[3] = mxCalloc((size_t)n_mareg, 16);
					txt_X  = mxCalloc((size_t)(n_mareg*10+4), 1);	txt_Y = mxCalloc((size_t)(n_mareg*10+4), 1);
					sprintf(txt[0], "#\t"); sprintf(txt[1], "#\t"); sprintf(txt[2], "#\t"); sprintf(txt[3], "#\t");
					sprintf(txt_X, "# X\t");	sprintf(txt_Y, "# Y\t");
					if (isGeog)
						strcpy(fmt, "\t%.5f");
					else
						strcpy(fmt, "\t%.2f");
					for (n = 0; n < n_mareg; n++) {
						ix = lcum_p[n] % nest.hdr[writeLevel].nx;
						iy = lcum_p[n] / nest.hdr[writeLevel].nx;
						sprintf(t0, "%8s",  mareg_names[n]);
						sprintf(t1, fmt, nest.hdr[writeLevel].x_min + ix * nest.hdr[writeLevel].x_inc);	/* Xs */
						sprintf(t2, fmt, nest.hdr[writeLevel].y_min + iy * nest.hdr[writeLevel].y_inc);	/* Ys */
						sprintf(t3, "\t%.1f", nest.bat[writeLevel][lcum_p[n]]); 	/* Zs (from grid) */
						strcat(txt[0], t0);		strcat(txt[1], t1);		strcat(txt[2], t2);		strcat(txt[3], t3);
						strcat(txt_X, t1);		strcat(txt_Y, t2);
					}
					fprintf(fp, "%s\n%s\n%s\n%s\n%s\n%s\n", txt[0], txt[1], txt[2], txt[3], txt_X, txt_Y);
					fprintf(fp, ">XY\n");		/* So that the file can be opened directly with dag-n-drop to Mirone */
					mxFree(txt[0]);	mxFree(txt[1]);	mxFree(txt[2]);	mxFree(txt[3]);	mxFree(txt_X);	mxFree(txt_Y);
				}
				fprintf (fp, "%.3f", (time_h + dt/2));
				if (out_maregs_velocity) {
					double vx, vy;
					for (ij = 0; ij < n_mareg; ij++) {
						if (htotal_for_maregs[lcum_p[ij]] > EPS2) {
							vx = vx_for_maregs[lcum_p[ij]];
							vy = vy_for_maregs[lcum_p[ij]];
						}
						else {vx = vy = 0;}
						t = fabs(eta_for_maregs[lcum_p[ij]]) < EPS2 ? 0 : 90 - atan2(vy, vx) * R2D;
						if (t < 0) t += 360;
						fprintf (fp, "\t%.5f\t%.2f\t%.2f\t%.1f", eta_for_maregs[lcum_p[ij]], vx, vy, t);
					}
				}
				else {
					for (ij = 0; ij < n_mareg; ij++)
						fprintf (fp, "\t%.5f", eta_for_maregs[lcum_p[ij]]);
				}
				fprintf (fp, "\n");
			}
		}

		if (do_tracers && k > 0) {
			int n;
			unsigned int ix, jy, itmp, ij_c;
			double vx, vy, vx1, vx2, vy1, vy2, dx, dy;
			double v_LLx, v_LLy, v_LRx, v_LRy, v_ULx, v_ULy, v_URx, v_URy;
			for (n = 0; n < n_oranges; n++) {
				ix = (int)((oranges[n].x[k-1] - nest.hdr[writeLevel].x_min) / nest.hdr[writeLevel].x_inc);
				jy = (int)((oranges[n].y[k-1] - nest.hdr[writeLevel].y_min) / nest.hdr[writeLevel].y_inc);
				dx = oranges[n].x[k-1] - (nest.hdr[writeLevel].x_min + ix * nest.hdr[writeLevel].x_inc);
				dy = oranges[n].y[k-1] - (nest.hdr[writeLevel].y_min + jy * nest.hdr[writeLevel].y_inc);

				ij_c = jy * nest.hdr[writeLevel].nx + ix;   /* Linear index LowerLeft cell corner */
				if (htotal_for_oranges[ij_c] > EPS2 && htotal_for_oranges[ij_c + 1] > EPS2) {
					v_LLx = vx_for_oranges[ij_c];		v_LLy = vy_for_oranges[ij_c];
					v_LRx = vx_for_oranges[ij_c+1];		v_LRy = vy_for_oranges[ij_c+1];
				}
				else
					v_LLx = v_LLy = v_LRx = v_LRy = 0;

				ij_c += nest.hdr[writeLevel].nx;            /* Linear index UpperLeft cell corner */
				if (htotal_for_oranges[ij_c] > EPS2 && htotal_for_oranges[ij_c + 1] > EPS2) {
					v_ULx = vx_for_oranges[ij_c];		v_ULy = vy_for_oranges[ij_c];
					v_URx = vx_for_oranges[ij_c+1];		v_URy = vy_for_oranges[ij_c+1];
				}
				else
					v_ULx = v_ULy = v_URx = v_URy = 0;

				dx /= nest.hdr[writeLevel].x_inc;		/* Resuse the dx,dy variables */
				dy /= nest.hdr[writeLevel].y_inc;

				vx1 = v_LLx + (v_LRx - v_LLx) * dx;		vx2 = v_ULx + (v_URx - v_ULx) * dx;
				vy1 = v_LLy + (v_ULy - v_LLy) * dy;		vy2 = v_LRy + (v_URy - v_LRy) * dy;
				vx  = vx1 + (vx2 - vx1) * dy;			vy  = vy1 + (vy2 - vy1) * dx;

				oranges[n].x[k] = oranges[n].x[k-1] + vx * dt;
				oranges[n].y[k] = oranges[n].y[k-1] + vy * dt;
			}
		}

		/* ------------------------------------------------------------------------------------ */
		/* -- This chunk deals with the cases where we compute something at every step
		      but write only one grid at the end of all cycles
		/* ------------------------------------------------------------------------------------ */
		if (max_level)			/* Output max surface level. This is only executed when writing mother grid */
			update_max(&nest);
		else if (max_energy) {
			if (k % decimate_max == 0) {
				total_energy(&nest, workMax, writeLevel);
				for (ij = 0; ij < nest.hdr[writeLevel].nm; ij++)
					if (wmax[ij] < workMax[ij]) wmax[ij] = workMax[ij];
			}
		}
		else if (max_power) {
			if (k % decimate_max == 0) {
				power(&nest, workMax, writeLevel);
				for (ij = 0; ij < nest.hdr[writeLevel].nm; ij++)
					if (wmax[ij] < workMax[ij]) wmax[ij] = workMax[ij];
			}
		}
		
		if (max_velocity)       /* Output max velocity. This is only executed when writing mother grid */
			update_max_velocity(&nest);

		if (k == (n_of_cycles - 1)) {   /* Last cycle: write wmax to file */
			size_t len = strlen(stem) - 1;
			while (stem[len] != '.' && len > 0) len--;
			if (do_maxs) {              /* Deal with the case of 'only one of the maximums' */
				if (len == 0) {                    /* No extension, add a "_max.grd" one */
					strcpy(prenome, stem);
					strcat(prenome, "_max.grd");
				}
				else {
					strncpy(prenome, stem, len);
					strcat(prenome, "_max.grd");
					//strcat(prenome, &stem[len]);    /* Put back the given extension */
				}

				write_grd_bin(prenome, xMinOut, yMinOut, dx, dy, i_start, j_start, i_end, j_end,
				              nest.hdr[writeLevel].nx, wmax);
			}

			if (nest.do_long_beach) {           /* In this case the calculations were done in mass() */
				for (ij = 0; ij < nest.hdr[writeLevel].nm; ij++)
					wmax[ij] = nest.long_beach[writeLevel][ij];	/* Implicitly convert from short int to float */

				write_grd_bin(fname_mask_lbeach, xMinOut, yMinOut, dx, dy, i_start, j_start, i_end, j_end,
				              nest.hdr[writeLevel].nx, wmax);
			}
			if (nest.do_short_beach) {          /* In this case the calculations were done in mass() */
				for (ij = 0; ij < nest.hdr[writeLevel].nm; ij++)
					wmax[ij] = nest.short_beach[writeLevel][ij];/* Implicitly convert from short int to float */

				write_grd_bin(fname_mask_sbeach, xMinOut, yMinOut, dx, dy, i_start, j_start, i_end, j_end,
				              nest.hdr[writeLevel].nx, wmax);
			}

			if (max_velocity || nest.do_max_velocity) { /* Maximum velocity is treated differently */
				for (ij = 0; ij < nest.hdr[writeLevel].nm; ij++)
					vmax[ij] = sqrt(vmax[ij]);          /* We had stored only v^2 */

				if (len == 0)                           /* No extension, add a "_max_speed.grd" one */
					strcat(strcpy(prenome, stem), "_max_speed.grd");
				else {
					strcat(strncpy(prenome, stem, len), "_max_speed");
					strcat(prenome, &stem[len]);        /* Put back the given extension */
				}
				write_grd_bin(prenome, xMinOut, yMinOut, dx, dy, i_start, j_start, i_end, j_end,
				              nest.hdr[writeLevel].nx, vmax);
			}
		}
		/* -------------------------------------------------------------------------------- */
 
		if (grn && time_h > time_jump && ((k % grn) == 0 || k == (n_of_cycles - 1)) ) {		/* If we are at a saving step */
			if (surf_level) {
				for (ij = 0; ij < nest.hdr[writeLevel].nm; ij++)
					work[ij] = (float)nest.etad[writeLevel][ij];
			}
			else if (water_depth) {
				for (ij = 0; ij < nest.hdr[writeLevel].nm; ij++) {
					work[ij] = (float)nest.etad[writeLevel][ij];
					if (nest.bat[writeLevel][ij] < 0) {
						if ((work[ij] = (float)(nest.etaa[writeLevel][ij] + nest.bat[writeLevel][ij])) < 0)
							work[ij] = 0;
					}
				}
			}

			if (out_energy)
				total_energy(&nest, work, writeLevel);
			else if (out_power)
				power(&nest, work, writeLevel);

			if (write_grids) {
				sprintf(prenome, "%s%05d.grd", stem, irint(time_h));
				write_grd_bin(prenome, xMinOut, yMinOut, dx, dy, i_start, j_start, i_end, j_end, nest.hdr[writeLevel].nx, work);
			}

			if (out_momentum && !out_3D) {
				if (stem[0] == 0)
					sprintf(prenome,"%.5d\0", irint(time_h) );
				else
					sprintf(prenome, "%s%.5d", stem, irint(time_h) );

				for (ij = 0; ij < nest.hdr[writeLevel].nm; ij++) work[ij] = (float)nest.fluxm_d[writeLevel][ij];

				write_grd_bin(strcat(prenome,"_Uh.grd"), xMinOut, yMinOut, dx, dy, 
				              i_start, j_start, i_end, j_end, nest.hdr[writeLevel].nx, work);

				for (ij = 0; ij < nest.hdr[writeLevel].nm; ij++) work[ij] = (float)nest.fluxn_d[writeLevel][ij];

				prenome[strlen(prenome) - 7] = '\0';	/* Remove the _Uh.grd' so that we can add '_Vh.grd' */
				write_grd_bin(strcat(prenome,"_Vh.grd"), xMinOut, yMinOut, dx, dy, 
				              i_start, j_start, i_end, j_end, nest.hdr[writeLevel].nx, work);
			}

			if (out_velocity && !out_3D) {
				sprintf(prenome, "%s%05d", stem, irint(time_h));

				if (out_velocity_x) {
					for (ij = 0; ij < nest.hdr[writeLevel].nm; ij++) {
						work[ij] = (nest.htotal_d[writeLevel][ij] > EPS2) ? (float)nest.vex[writeLevel][ij] : 0;
						if (nest.htotal_d[writeLevel][ij] < 0.5 && fabs(work[ij]) >= V_LIMIT)	/* Clip above this combination */
							work[ij] = 0;
					}

					write_grd_bin(strcat(prenome,"_U.grd"), xMinOut + nest.hdr[writeLevel].x_inc/2, yMinOut,
					              dx, dy, i_start, j_start, i_end, j_end, nest.hdr[writeLevel].nx, work);
					prenome[strlen(prenome)-6] = '\0';	/* Remove the _U.grd' so that we can add '_V.grd' */
				}
				if (out_velocity_y) {
					for (ij = 0; ij < nest.hdr[writeLevel].nm; ij++) {
						work[ij] = (nest.htotal_d[writeLevel][ij] > EPS2) ? (float)nest.vey[writeLevel][ij] : 0;
						if (nest.htotal_d[writeLevel][ij] < 0.5 && fabs(work[ij]) >= V_LIMIT)	/* Clip above this combination */
							work[ij] = 0;
					}

					write_grd_bin(strcat(prenome,"_V.grd"), xMinOut, yMinOut + nest.hdr[writeLevel].y_inc/2,
					              dx, dy, i_start, j_start, i_end, j_end, nest.hdr[writeLevel].nx, work);
				}
			}

#ifdef HAVE_NETCDF
			if (out_sww) {
				if (first_anuga_time) {
					time0 = time_h;
					first_anuga_time = FALSE;
				}
				time_for_anuga = time_h - time0;	/* I think ANUGA wants time starting at zero */
				err_trap (nc_put_vara_double (ncid, ids[6], &start0, &count0, &time_for_anuga));

				write_anuga_slice(&nest, ncid, ids[7], i_start, j_start, i_end, j_end, tmp_slice, start1_A, count1_A,
				                  stage_range, 1, with_land, writeLevel);
				write_anuga_slice(&nest, ncid, ids[9], i_start, j_start, i_end, j_end, tmp_slice, start1_A, count1_A,
				                  xmom_range, 2, with_land, writeLevel);
				write_anuga_slice(&nest, ncid, ids[11], i_start, j_start, i_end, j_end, tmp_slice, start1_A, count1_A,
				                  ymom_range, 3, with_land, writeLevel);

				start1_A[0]++;		/* Increment for the next slice */
			}

			if (out_most) {
				/* Here we'll use the start0 computed above */
				err_trap (nc_put_vara_double (ncid_most[0], ids_ha[4], &start0, &count0, &time_h));
				err_trap (nc_put_vara_double (ncid_most[1], ids_ua[4], &start0, &count0, &time_h));
				err_trap (nc_put_vara_double (ncid_most[2], ids_va[4], &start0, &count0, &time_h));

				write_most_slice(&nest, ncid_most, ids_most, i_start, j_start, i_end, j_end,
				                 tmp_slice, start1_M, count1_M, actual_range, TRUE, writeLevel);
				start1_M[0]++;		/* Increment for the next slice */
			}
			else if (out_3D) {
				/* Here we'll use the start0 computed above */
				err_trap(nc_put_vara_double(ncid_3D[0], ids_z[2], &start0, &count0, &time_h));
				write_most_slice(&nest, ncid_3D, ids_3D, i_start, j_start, i_end, j_end,
				                 work, start1_M, count1_M, actual_range, FALSE, writeLevel);
				start1_M[0]++;		/* Increment for the next slice */
			}
#endif

			start0++;			/* Only used with netCDF formats */
		}
		time_h += dt;
		nest.time_h = time_h;
	}
	/* ------------------------------- END MAIN LOOP --------------------------------------- */

#ifdef HAVE_NETCDF
	if (out_sww) {          /* Uppdate range values and close SWW file */
		err_trap(nc_put_var_float(ncid, ids[8], stage_range));
		err_trap(nc_put_var_float(ncid, ids[10], xmom_range));
		err_trap(nc_put_var_float(ncid, ids[12], ymom_range));
		err_trap(nc_close (ncid)); 
	}

	if (out_most) {         /* Close MOST files */
		err_trap(nc_close(ncid_most[0]));
		err_trap(nc_close(ncid_most[1]));
		err_trap(nc_close(ncid_most[2]));
	}
	else if (out_3D) {      /* Uppdate range values and close 3D file */
		err_trap(nc_put_att_double(ncid_3D[0], ids_z[3], "actual_range", NC_DOUBLE, 2U, actual_range));
		if (out_velocity_x)
			err_trap(nc_put_att_double(ncid_3D[0], ids_z[5], "actual_range", NC_DOUBLE, 2U, &actual_range[2]));
		if (out_velocity_y)
			err_trap(nc_put_att_double(ncid_3D[0], ids_z[6], "actual_range", NC_DOUBLE, 2U, &actual_range[4]));

		/* It's ugly to have this here but I have no means to tell write_most_slice() when it's the last call */
		if (nest.do_long_beach || nest.do_short_beach) {	/* Write the mask(s) */
			unsigned char *pchar = NULL;
			size_t	start_b[2] = {0,0}, count_b[2];
			count_b[0] = nest.hdr[writeLevel].ny;	count_b[1] = nest.hdr[writeLevel].nx;
			/* Allocate memory for the mask that were stored in floats */
			if ((pchar = (unsigned char *) mxCalloc((size_t)nest.hdr[writeLevel].nm, sizeof(unsigned char)) ) == NULL)
				{no_sys_mem("(ShortBeach)", nest.hdr[writeLevel].nm); Return(-1);}
			if (nest.do_long_beach) {
				float act_range[2] = {0, 0};
				for (ij = 0; ij < nest.hdr[writeLevel].nm; ij++) pchar[ij] = (unsigned char)nest.long_beach[writeLevel][ij];
				for (ij = 0; ij < nest.hdr[writeLevel].nm; ij++) if (pchar[ij] == 1) {act_range[1] = 1; break;}
				err_trap(nc_put_att_float (ncid_3D[0], ids_z[7], "actual_range", NC_FLOAT, 2U, act_range));
				err_trap(nc_put_vara_ubyte(ncid_3D[0], ids_z[7], start_b, count_b, pchar));		/* Write the mask */
			}
			if (nest.do_short_beach) {
				float act_range[2] = {0, 0};
				for (ij = 0; ij < nest.hdr[writeLevel].nm; ij++) pchar[ij] = (unsigned char)nest.short_beach[writeLevel][ij];
				for (ij = 0; ij < nest.hdr[writeLevel].nm; ij++) if (pchar[ij] == 1) {act_range[1] = 1; break;}
				err_trap(nc_put_att_float (ncid_3D[0], ids_z[8], "actual_range", NC_FLOAT, 2U, act_range));
				err_trap(nc_put_vara_ubyte(ncid_3D[0], ids_z[8], start_b, count_b, pchar));		/* Write the mask */
			}
			if (pchar != NULL) mxFree(pchar);
		}

		err_trap(nc_close(ncid_3D[0])); 
	}

	if (out_sww || out_most) mxFree ((void *)tmp_slice);

	if (out_maregs_nc) {    /* Write the maregs in a netCDF file */
		if (do_Kaba) {
			int    k, kp, km, nKabas, RC[2];
			size_t strt, cnt, row, col;
			double x1, x2, y1, y2, BB[8];

			/* Need to change order from scanline to columnwise (one mareg, than next and so on) before saving to nc */
			nKabas = KbGridRows * KbGridCols;
			for (km = k = 0; km < n_mareg; km++)
				for (kp = 0; kp < count_time_maregs_timeout; kp++)
					maregs_array_t[k++] = maregs_array[kp*n_mareg + km];

			if (cntKabas == 0) {    		/* First call. Open nc file and save first slice */
				count_Mar[1] = count_time_maregs_timeout * n_mareg;	/* Was not yet initialized */
				ncid_Mar = write_greens_nc(&nest, hcum, maregs_array_t, start_Mar, count_Mar,
				                           maregs_timeout, lcum_p, mareg_names, history, ids_Mar,
				                           n_mareg, count_time_maregs_timeout, writeLevel);
				sprintf(txt, "%g/%g/%g/%g", kaba_xmin, kaba_xmax, kaba_ymin, kaba_ymax);	/* Region string */
				BB[0] = kaba_xmin;			BB[2] = kaba_ymin;
			}
			else {
				start_Mar[0]++;         /* Increment for the next slice */
				err_trap(nc_put_vara_float(ncid_Mar, ids_Mar[4], start_Mar, count_Mar, maregs_array_t));
			}
			cntKabas++;
			col = cntKabas % KbGridCols;		row = cntKabas / KbGridCols;
			x1 = kaba_xmin + col * dxKb;		x2 = kaba_xmax + col * dxKb;
			y1 = kaba_ymin + row * dyKb;		y2 = kaba_ymax + row * dyKb;
			strt = (size_t)(cntKabas - 1);
			//err_trap(nc_put_vara_int(ncid_Mar, ids_Mar[2], &strt, &count0, &cntKabas));	/* Update unlimited var */

			if (cntKabas < nKabas) {	/* While not all nodes in KabaGrid GOTO ... */
				unsigned int nm, lev;
				sprintf(txt, "%g/%g/%g/%g", x1, x2, y1, y2);	/* Region string to be stored in the nc file */
				kaba_source(hdr_b, dx, dy, x1, x2, y1, y2, do_Kaba, nest.etaa[0]);
				/* --------------------------------- Reset these ----------------------------------*/
				count_maregs_timeout = 0;	count_time_maregs_timeout = 0;	nest.time_h = time_h = 0;
				for (lev = 0; lev <= num_of_nestGrids; lev++) {
					nm = nest.hdr[lev].nm;
					memset(nest.etad[lev],     0, (size_t)(nm * sizeof(double)));
					memset(nest.fluxm_a[lev],  0, (size_t)(nm * sizeof(double)));
					memset(nest.fluxm_d[lev],  0, (size_t)(nm * sizeof(double)));
					memset(nest.fluxn_a[lev],  0, (size_t)(nm * sizeof(double)));
					memset(nest.fluxn_d[lev],  0, (size_t)(nm * sizeof(double)));
					memset(nest.htotal_a[lev], 0, (size_t)(nm * sizeof(double)));
					memset(nest.htotal_d[lev], 0, (size_t)(nm * sizeof(double)));
				}
				/* ------------------------------------------------------------------------------- */
				fprintf(stderr, "Computing prism %d out of %d (row = %d\tcol = %d)\t%s\n",
				        cntKabas+1, KbGridRows * KbGridCols, row+1, col+1, txt);
				goto LoopKabas;			/* A LOOP HERE */
			}
			BB[1] = kaba_xmin + KbGridCols*dxKb;			BB[3] = kaba_ymin + KbGridRows*dyKb;
			BB[4] = dxKb;           BB[5] = dyKb;
			BB[6] = KbGridRows;     BB[7] = KbGridCols;
			err_trap(nc_put_att_double(ncid_Mar,  ids_Mar[4], "BB_inc_RC", NC_DOUBLE, 8U, BB));
			err_trap(nc_close(ncid_Mar)); 
		}
		else
			write_maregs_nc(&nest, hcum, maregs_array, maregs_timeout, lcum_p, mareg_names,
			                history, n_mareg, count_time_maregs_timeout, writeLevel);

		mxFree(maregs_array);
		mxFree(maregs_array_t);
		mxFree(maregs_timeout);
	}
#endif
	
	if (do_tracers) {			/* Write the tracers file and free memory */
		for (k = 0; k < n_of_cycles; k++) {
			fprintf(fp_oranges, "%.2f", k * dt);
			for (n = 0; n < n_oranges; n++)
				fprintf(fp_oranges, "\t%.5f\t%.5f", oranges[n].x[k], oranges[n].y[k]);

			fprintf(fp_oranges, "\n");
		}

		fclose (fp_oranges);
		for (n = 0; n < n_oranges; n++) {
			mxFree(oranges[n].x);		mxFree(oranges[n].y);
		}
	}

#ifdef I_AM_MEX
	if (!IamCompiled) {
		*ptr_wb = 1.0;
		mexCallMATLAB(0,NULL,1,rhs,"aguentabar");
	}
	else
		mexEvalString("aguentabar(1.0)");

	/* Clean up allocated memory. */
	mxDestroyArray(rhs[0]);		mxDestroyArray(rhs[1]);		mxDestroyArray(rhs[2]);
#else
	fprintf(stderr, "\t100 %%\tCPU secs/ticks = %.3f\n", (double)(clock() - tic));
#endif

	if (cumpt) {
		fclose (fp);
		if (cum_p) mxFree((void *) cum_p);
		if (time_p)mxFree((void *) time_p);	 
	}

	free_arrays(&nest, isGeog, num_of_nestGrids);
	if (vmax) mxFree (vmax);
	if (wmax) mxFree (wmax);
	if (lcum_p) mxFree (lcum_p);
	if (workMax) mxFree (workMax);
	if (work) mxFree (work);
	if (mareg_names) {
		for (k = 0; k < n_mareg; k++) free(mareg_names[k]);	/* They were allocated with strdup() */
		mxFree(mareg_names);
	}

#ifndef I_AM_MEX
	return 0;
#endif
}

/* -------------------------------------------------------------------------- */
void sanitize_nestContainer(struct nestContainer *nest) {
	int i;

	nest->do_upscale     = FALSE;
	nest->do_long_beach  = FALSE;
	nest->do_short_beach = FALSE;
	nest->do_linear      = FALSE;
	nest->do_max_level   = FALSE;
	nest->do_max_velocity= FALSE;
	nest->out_velocity_x = FALSE;
	nest->out_velocity_y = FALSE;
	nest->do_Coriolis    = FALSE;
	nest->bnc_var_nTimes = 0;
	nest->bnc_pos_nPts   = 0;
	nest->bnc_border[0]  = nest->bnc_border[1] = nest->bnc_border[2] = nest->bnc_border[3] = FALSE;
	nest->run_jump_time  = 0;
	nest->lat_min4Coriolis = -100;
	nest->manning_depth = 8000;   /* Default, if manning, and use already the z pos down */
	nest->bnc_pos_x = NULL;
	nest->bnc_pos_y = NULL;
	nest->bnc_var_t = NULL;
	nest->bnc_var_z = NULL;
	nest->bnc_var_zTmp = NULL;
	for (i = 0; i < 10; i++) {
		nest->level[i] = -1;      /* Will be set to the due level number for existing nesting levels */
		nest->manning[i] = 0;
		nest->LLrow[i] = nest->LLcol[i] = nest->ULrow[i] = nest->ULcol[i] =
		nest->URrow[i] = nest->URcol[i] = nest->LRrow[i] = nest->LRcol[i] =
		nest->incRatio[i] = 0;
		nest->LLx[i] = nest->LLy[i] = nest->ULx[i] = nest->ULy[i] =
		nest->URx[i] = nest->URy[i] = nest->LRx[i] = nest->LRy[i] = 0;
		nest->dt[i] = 0;
		nest->long_beach[i]  = NULL;
		nest->short_beach[i] = NULL;
		nest->bat[i] = NULL;
		nest->fluxm_a[i] = nest->fluxm_d[i] = NULL;
		nest->fluxn_a[i] = nest->fluxm_d[i] = NULL;
		nest->htotal_a[i] = nest->htotal_d[i] = NULL;
		nest->etaa[i] = nest->etad[i] = NULL;
		nest->vex[i] = nest->vey[i] = NULL;
		nest->edge_col[i] = nest->edge_colTmp[i] = NULL;
		nest->edge_row[i] = nest->edge_rowTmp[i] = NULL;
		nest->edge_col_P[i] = nest->edge_col_Ptmp[i] = NULL;
		nest->edge_row_P[i] = nest->edge_row_Ptmp[i] = NULL;
	}
}

/* -------------------------------------------------------------------------- */
int check_paternity(struct nestContainer *nest) {
	/* Check if descendent grid with qualifies as nested grid with respect to grid HDR_PARENT
	   WARNING: everything here assumes headers are in GRID registration.
	*/

	int error = 0, k = 1;	/* 1 because first nested grid is Level 1 */
	double suggest;

	while (nest->level[k] > 0) {
		/* Check nesting at LowerLeft corner */
		if (check_binning(nest->hdr[k-1].x_min, nest->hdr[k].x_min, nest->hdr[k-1].x_inc,
		                  nest->hdr[k].x_inc, nest->hdr[k-1].x_inc / 4, &suggest)) {
			mexPrintf("Lower left corner of doughter grid does not obey to the nesting rules.\n"
				"X_MIN should be (in grid registration):\n\t%f\n", suggest);
			error++;
		}
		if (check_binning(nest->hdr[k-1].y_min, nest->hdr[k].y_min, nest->hdr[k-1].y_inc,
		                  nest->hdr[k].y_inc, nest->hdr[k-1].y_inc / 4, &suggest)) {
			mexPrintf("Lower left corner of doughter grid does not obey to the nesting rules.\n"
				"Y_MIN should be (in grid registration):\n\t%f\n", suggest);
			error++;
		}
		/* Check nesting at UpperRight corner */
		if (check_binning(nest->hdr[k-1].x_min, nest->hdr[k].x_max, nest->hdr[k-1].x_inc,
		                  -nest->hdr[k].x_inc, nest->hdr[k-1].x_inc / 4, &suggest)) {
			mexPrintf("Upper right corner of doughter grid does not obey to the nesting rules.\n"
				"X_MAX should be (in grid registration):\n\t%f\n", suggest);
			error++;
		}
		if (check_binning(nest->hdr[k-1].y_min, nest->hdr[k].y_max, nest->hdr[k-1].y_inc,
		                  -nest->hdr[k].y_inc, nest->hdr[k-1].y_inc / 4, &suggest)) {
			mexPrintf("Upper right corner of doughter grid does not obey to the nesting rules.\n"
				"Y_MAX should be (in grid registration):\n\t%f\n", suggest);
			error++;
		}

		if (error)		/* Abort since any further info would be false/useless */
			break;
	}
	return (error);
}

/* -------------------------------------------------------------------------- */
int check_binning(double x0P, double x0D, double dxP, double dxD, double tol, double *suggest) {
	/* Check that point X0D of doughter grid fits within tolerance TOL into parent grid
	   Use a negative dxD when checking the upper left corner (or any east/north edges)
	*/
	int n_incs;
	double x, dec;

	x = (x0D - x0P) / dxP;
	n_incs = rint(x);
	dec = (x0D - (x0P + n_incs * dxP));
	if (fabs(dec - (dxP / 2 + dxD / 2)) > tol) {
		*suggest = x0P + n_incs * dxP + dxP / 2 + dxD / 2;		/* Suggested location for x0D */
		return (-1);
	}
	return(0);
}

/* -------------------------------------------------------------------------- */
int initialize_nestum(struct nestContainer *nest, int isGeog, int lev) {
	/* Initialize the nest struct. */

	int row, col, i, nSizeIncX, nSizeIncY, n;
	unsigned int nm = nest->hdr[lev].nm;
	double dt, scale;
	double xoff, yoff, xoff_P, yoff_P;		/* Offsets to move from grid to pixel registration (zero if grid in pix reg) */
	struct grd_header hdr = nest->hdr[lev-1];

	if (lev > 0) {
		/* -------------------- Check that this grid is nestifiable -------------------- */
		nSizeIncX = irint(hdr.x_inc / nest->hdr[lev].x_inc);
		if ((hdr.x_inc / nest->hdr[lev].x_inc) - nSizeIncX > 1e-5) {
			mexPrintf("NSWING ERROR: X increments of inner (%d) and outer (%d) grids are incompatible.\n", lev, lev-1);
			mexPrintf("\tInteger ratio of parent (%d) to doughter (%d) X increments = %d\n", lev, lev-1, nSizeIncX);
			mexPrintf("\tActual  ratio as a floating point = %f\n\tDifference between the two cannot exceed 1e-5\n",
			          hdr.x_inc / nest->hdr[lev].x_inc);
			return(-1);
		}

		nSizeIncY = irint(hdr.y_inc / nest->hdr[lev].y_inc);
		if ((hdr.y_inc / nest->hdr[lev].y_inc) - nSizeIncY > 1e-5) {
			mexPrintf("NSWING ERROR: Y increments of inner (%d) and outer (%d) grids are incompatible.\n", lev, lev-1);
			mexPrintf("\tInteger ratio of parent (%d) to doughter (%d) Y increments = %d\n", lev, lev-1, nSizeIncY);
			mexPrintf("\tActual  ratio as a floating point = %f\n\tDifference between the two cannot exceed 1e-5\n",
			          hdr.y_inc / nest->hdr[lev].y_inc);
			return(-1);
		}

		if (nSizeIncX != nSizeIncY) {
			mexPrintf("NSWING ERROR: X/Y increments of inner (%d) and outer (%d) grid do not divide equaly.\n", lev, lev-1);
			mexPrintf("\tinc_x(%d) = %f\t inc_x(%d) = %f.\n", lev-1, hdr.x_inc, lev, nest->hdr[lev].x_inc);
			mexPrintf("\tinc_y(%d) = %f\t inc_y(%d) = %f.\n", lev-1, hdr.y_inc, lev, nest->hdr[lev].y_inc);
			mexPrintf("\tRatio of X increments (round(inc_x(%d) / inc_x(%d)) = %d.\n", lev-1, lev, nSizeIncX);
			mexPrintf("\tRatio of Y increments (round(inc_y(%d) / inc_y(%d)) = %d.\n", lev-1, lev, nSizeIncY);
			return(-1);
		}

		nest->incRatio[lev] = nSizeIncX;
		/* ----------------------------------------------------------------------------- */

		/* Compute the run time step interval for this level */
		scale = (isGeog) ? 111000 : 1;		/* To get the incs in meters */
		dt = 0.5 * MIN(nest->hdr[lev].x_inc, nest->hdr[lev].y_inc) * scale / sqrt(NORMAL_GRAV * fabs(nest->hdr[lev].z_min));
		nest->dt[lev] = nest->dt[lev-1] / ceil(nest->dt[lev-1] / dt);
	}

	nest->level[lev] = lev;

	/* Allocate the working arrays */
	if (nest->bat[lev] == NULL && (nest->bat[lev] = (double *)  mxCalloc ((size_t)nm, sizeof(double)) ) == NULL)
		{no_sys_mem("(bat)", nm); return(-1);}

	if ((nest->etaa[lev] = (double *)     mxCalloc ((size_t)nm, sizeof(double)) ) == NULL)
		{no_sys_mem("(etaa)", nm); return(-1);}
	if ((nest->etad[lev] = (double *)     mxCalloc ((size_t)nm, sizeof(double)) ) == NULL)
		{no_sys_mem("(etad)", nm); return(-1);}
	if ((nest->fluxm_a[lev] = (double *)  mxCalloc ((size_t)nm, sizeof(double)) ) == NULL)
		{no_sys_mem("(fluxm_a)", nm); return(-1);}
	if ((nest->fluxm_d[lev] = (double *)  mxCalloc ((size_t)nm, sizeof(double)) ) == NULL)
		{no_sys_mem("(fluxm_d)", nm); return(-1);}
	if ((nest->fluxn_a[lev] = (double *)  mxCalloc ((size_t)nm, sizeof(double)) ) == NULL)
		{no_sys_mem("(fluxn_a)", nm); return(-1);}
	if ((nest->fluxn_d[lev] = (double *)  mxCalloc ((size_t)nm, sizeof(double)) ) == NULL)
		{no_sys_mem("(fluxn_d)", nm); return(-1);}
	if ((nest->htotal_a[lev] = (double *) mxCalloc ((size_t)nm, sizeof(double)) ) == NULL)
		{no_sys_mem("(htotal_a)", nm); return(-1);}
	if ((nest->htotal_d[lev] = (double *) mxCalloc ((size_t)nm, sizeof(double)) ) == NULL)
		{no_sys_mem("(htotal_d)", nm); return(-1);}

	if (nest->do_long_beach && (lev == nest->writeLevel)) {
		if ((nest->long_beach[lev] = (short int *) mxCalloc ((size_t)nm, sizeof(short int)) ) == NULL)
			{no_sys_mem("(long_beach)", nm); return(-1);}
	}
	if (nest->do_short_beach && (lev == nest->writeLevel)) {
		if ((nest->short_beach[lev] = (short int *) mxCalloc ((size_t)nm, sizeof(short int)) ) == NULL)
			{no_sys_mem("(short_beach)", nm); return(-1);}
	}

	if (nest->out_velocity_x && (lev == nest->writeLevel)) {
		if ((nest->vex[lev] = (double *) mxCalloc ((size_t)nm, sizeof(double)) ) == NULL)
			{no_sys_mem("(vex)", nm); return(-1);}
	}
	if (nest->out_velocity_y && (lev == nest->writeLevel)) {
		if ((nest->vey[lev] = (double *) mxCalloc ((size_t)nm, sizeof(double)) ) == NULL)
			{no_sys_mem("(vey)", nm); return(-1);}
	}

	n = nest->hdr[lev].ny;
	if (isGeog == 1) {		/* case spherical coordinates  */
		if ((nest->r0[lev] = (double *)  mxCalloc ((size_t)n,	sizeof(double)) ) == NULL) 
			{no_sys_mem("(r0)", n); return(-1);}
		if ((nest->r1m[lev] = (double *) mxCalloc ((size_t)n,	sizeof(double)) ) == NULL) 
			{no_sys_mem("(r1m)", n); return(-1);}
		if ((nest->r1n[lev] = (double *) mxCalloc ((size_t)n,	sizeof(double)) ) == NULL) 
			{no_sys_mem("(r1n)", n); return(-1);}
		if ((nest->r2m[lev] = (double *) mxCalloc ((size_t)n,	sizeof(double)) ) == NULL) 
			{no_sys_mem("(r2m)", n); return(-1);}
		if ((nest->r2n[lev] = (double *) mxCalloc ((size_t)n,	sizeof(double)) ) == NULL) 
			{no_sys_mem("(r2n)", n); return(-1);}
		if ((nest->r3m[lev] = (double *) mxCalloc ((size_t)n,	sizeof(double)) ) == NULL) 
			{no_sys_mem("(r3m)", n); return(-1);}
		if ((nest->r3n[lev] = (double *) mxCalloc ((size_t)n,	sizeof(double)) ) == NULL) 
			{no_sys_mem("(r3n)", n); return(-1);}
	}
	/* This may be used also by the Cartesian case */
	if ((nest->r4m[lev] = (double *) mxCalloc ((size_t)n,	sizeof(double)) ) == NULL) 
		{no_sys_mem("(r4m)", n); return(-1);}
	if ((nest->r4n[lev] = (double *) mxCalloc ((size_t)n,	sizeof(double)) ) == NULL) 
		{no_sys_mem("(r4n)", n); return(-1);}

	nest->bnc_pos_x = nest->bnc_pos_y = nest->bnc_var_t = NULL;
	nest->bnc_var_z = NULL;
	nest->bnc_var_zTmp = nest->bnc_var_z_interp = NULL;

	/* ------------------------------------------------------------------------------------------ */
	if (lev == 0)			/* All done for now */
		return(0);
	/* ------------------------------------------------------------------------------------------ */

	/* These two must be set to zero if inner grid was already pixel registrated */
	xoff = nest->hdr[lev].x_inc / 2;
	yoff = nest->hdr[lev].y_inc / 2;
	xoff_P = nest->hdr[lev-1].x_inc / 2;
	yoff_P = nest->hdr[lev-1].y_inc / 2;

	/* Compute the 4 coorners coordinates of the nodes on the parent grid embracing the nested grid */
	nest->LLx[lev] = (nest->hdr[lev].x_min - xoff) - hdr.x_inc / 2;
	nest->LLy[lev] = (nest->hdr[lev].y_min - yoff) - hdr.y_inc / 2;
	nest->ULx[lev] = (nest->hdr[lev].x_min - xoff) - hdr.x_inc / 2;
	nest->ULy[lev] = (nest->hdr[lev].y_max + yoff) + hdr.y_inc / 2;
	nest->URx[lev] = (nest->hdr[lev].x_max + xoff) + hdr.x_inc / 2;
	nest->URy[lev] = (nest->hdr[lev].y_max + yoff) + hdr.y_inc / 2;
	nest->LRx[lev] = (nest->hdr[lev].x_max + xoff) + hdr.x_inc / 2;
	nest->LRy[lev] = (nest->hdr[lev].y_min - yoff) - hdr.y_inc / 2;

	/* The row e column indices corresponding to above computed coordinates */
	nest->LLrow[lev] = irint((nest->LLy[lev] - hdr.y_min) / hdr.y_inc);
	nest->LLcol[lev] = irint((nest->LLx[lev] - hdr.x_min) / hdr.x_inc);
	nest->ULrow[lev] = irint((nest->ULy[lev] - hdr.y_min) / hdr.y_inc);
	nest->ULcol[lev] = irint((nest->ULx[lev] - hdr.x_min) / hdr.x_inc);
	nest->URrow[lev] = irint((nest->URy[lev] - hdr.y_min) / hdr.y_inc);
	nest->URcol[lev] = irint((nest->URx[lev] - hdr.x_min) / hdr.x_inc);
	nest->LRrow[lev] = irint((nest->LRy[lev] - hdr.y_min) / hdr.y_inc);
	nest->LRcol[lev] = irint((nest->LRx[lev] - hdr.x_min) / hdr.x_inc);

	/* Allocate vectors of the size of side inner grid to hold the BC */
	n = nest->hdr[lev].nx;
	nest->edge_rowTmp[lev] = (double *) mxCalloc((size_t)n, sizeof(double));	/* To be filled by interp */
	nest->edge_row[lev]    = (double *) mxCalloc((size_t)n, sizeof(double));
	/* Compute XXs of nested grid along N/S edge. We'll use only the south border coordinates */
	for (col = 0; col < n; col++)
		nest->edge_row[lev][col] = nest->hdr[lev].x_min + xoff + col * nest->hdr[lev].x_inc;

	n = nest->hdr[lev].ny;
	nest->edge_colTmp[lev] = (double *) mxCalloc((size_t)n, sizeof(double)); /* To be filled by interp */
	nest->edge_col[lev]    = (double *) mxCalloc((size_t)n, sizeof(double));
	for (row = 0; row < n; row++)		/* Compute YYs of inner grid along W/E edge */
		nest->edge_col[lev][row] = nest->hdr[lev].y_min + yoff + row * nest->hdr[lev].y_inc;

	/* These two will be used to make copies of data around the connected boundary on parent grid */
	n = nest->LRcol[lev] - nest->LLcol[lev] + 1;
	nest->edge_row_Ptmp[lev] = (double *) mxCalloc((size_t)n, sizeof(double));
	nest->edge_row_P[lev]    = (double *) mxCalloc((size_t)n, sizeof(double));
	for (i = 0; i < n; i++)
		nest->edge_row_P[lev][i] = nest->LLx[lev] + xoff_P + i * hdr.x_inc;      /* XX coords of parent grid along N/S edge */

	n = nest->ULrow[lev] - nest->LLrow[lev] + 1;
	nest->edge_col_Ptmp[lev] = (double *) mxCalloc((size_t)n, sizeof(double));
	nest->edge_col_P[lev]    = (double *) mxCalloc((size_t)n, sizeof(double));
	for (i = 0; i < n; i++)
		nest->edge_col_P[lev][i] = nest->LLy[lev] + xoff_P + i * hdr.y_inc;     /* YY coords of parent grid along W/E edge */

	/* ------------------- PRECISA REVISÃO MAS TAMBÉM PRECISA INICIALIZAR --------------- */
	nest->hdr[lev].lat_min4Coriolis = 0;
	nest->hdr[lev].doCoriolis = hdr.doCoriolis;

	return(0);
}

/* --------------------------------------------------------------------------- */
void free_arrays(struct nestContainer *nest, int isGeog, int lev) {
	int i;

	for (i = 0; i <= lev; i++) {
		if (nest->long_beach[i])  mxFree(nest->long_beach[i]);
		if (nest->short_beach[i]) mxFree(nest->short_beach[i]);
		if (nest->bat[i]) mxFree(nest->bat[i]);
		if (nest->vex[i]) mxFree(nest->vex[i]);
		if (nest->vey[i]) mxFree(nest->vey[i]);
		if (nest->etaa[i]) mxFree(nest->etaa[i]);
		if (nest->etad[i]) mxFree(nest->etad[i]);
		if (nest->fluxm_a[i]) mxFree(nest->fluxm_a[i]);
		if (nest->fluxm_d[i]) mxFree(nest->fluxm_d[i]);
		if (nest->fluxn_a[i]) mxFree(nest->fluxn_a[i]);
		if (nest->fluxn_d[i]) mxFree(nest->fluxn_d[i]);
		if (nest->htotal_a[i]) mxFree(nest->htotal_a[i]);
		if (nest->htotal_d[i]) mxFree(nest->htotal_d[i]);

		if (nest->edge_rowTmp[i]) mxFree(nest->edge_rowTmp[i]);
		if (nest->edge_row_Ptmp[i]) mxFree(nest->edge_row_Ptmp[i]);
		if (nest->edge_row_P[i]) mxFree(nest->edge_row_P[i]);
		if (nest->edge_row[i]) mxFree(nest->edge_row[i]);
		if (nest->edge_colTmp[i]) mxFree(nest->edge_colTmp[i]);
		if (nest->edge_col_Ptmp[i]) mxFree(nest->edge_col_Ptmp[i]);
		if (nest->edge_col_P[i]) mxFree(nest->edge_col_P[i]);
		if (nest->edge_col[i]) mxFree(nest->edge_col[i]);

		if (isGeog == 1) {
			if (nest->r0[i]) mxFree(nest->r0[i]);
			if (nest->r1m[i]) mxFree(nest->r1m[i]);
			if (nest->r1n[i]) mxFree(nest->r1n[i]);
			if (nest->r2m[i]) mxFree(nest->r2m[i]);
			if (nest->r2n[i]) mxFree(nest->r2n[i]);
			if (nest->r3m[i]) mxFree(nest->r3m[i]);
			if (nest->r3n[i]) mxFree(nest->r3n[i]);
		}
		if (nest->r4m[i]) mxFree(nest->r4m[i]);		/* Were allocated in any case */
		if (nest->r4n[i]) mxFree(nest->r4n[i]);
	}
	if (nest->bnc_pos_x) mxFree(nest->bnc_pos_x);
	if (nest->bnc_pos_y) mxFree(nest->bnc_pos_y);
	if (nest->bnc_var_z) mxFree(nest->bnc_var_z);
	if (nest->bnc_var_t) mxFree(nest->bnc_var_t);
	if (nest->bnc_var_zTmp) mxFree(nest->bnc_var_zTmp);
	if (nest->bnc_var_z_interp) mxFree(nest->bnc_var_z_interp);
}

/* --------------------------------------------------------------------------- */
void power(struct nestContainer *nest, float *work, int lev) {
	/* Compute tsunami wave power according to P = 1/2*rho*D*U*u^2 = 1/2*rho*D*sqrt(g*D)*u^2
	   http://plus.maths.org/content/tsunami-1
	   Note that since we don't have 'u' directly but must get from momentum we drop one 'D'
	   in the above formula.
	*/
	unsigned int ij;
	for (ij = 0; ij < nest->hdr[lev].nm; ij++) {
		if (nest->htotal_d[lev][ij] > EPS2) {
			work[ij] = (float)(( sqrt(nest->htotal_d[lev][ij] * NORMAL_GRAV) *
			                   ((nest->fluxm_d[lev][ij] * nest->fluxm_d[lev][ij]) +
			                    (nest->fluxn_d[lev][ij] * nest->fluxn_d[lev][ij])) /
			                     nest->htotal_d[lev][ij] ) * 500);
		}
	}
}

/* --------------------------------------------------------------------------- */
void total_energy(struct nestContainer *nest, float *work, int lev) {
	/* Compute wave total energy according to 1/2*rho*H*u^2 + 1/2*rho*g*eta^2
	   http://rspa.royalsocietypublishing.org/content/465/2103/725.full.pdf 
	   http://arxiv.org/ct?url=http%3A%2F%2Fdx.doi.org%2F10%252E1098%2Frspa%252E2008%252E0332&v=9ed92a65 
	*/
	unsigned int ij;

	for (ij = 0; ij < nest->hdr[lev].nm; ij++) {
		if (nest->htotal_d[lev][ij] > EPS2) {
			work[ij] = (float)(( nest->etad[lev][ij] * nest->etad[lev][ij] * NORMAL_GRAV +
			                   ((nest->fluxm_d[lev][ij] * nest->fluxm_d[lev][ij]) +
			                    (nest->fluxn_d[lev][ij] * nest->fluxn_d[lev][ij])) /
			                     nest->htotal_d[lev][ij] ) * 500);
		}
	}
}

/* --------------------------------------------------------------------------- */
int write_grd_bin(char *name, double x_min, double y_min, double x_inc, double y_inc, unsigned int i_start,
	unsigned int j_start, unsigned int i_end, unsigned int j_end, unsigned int nX, float *work) {

	/* Writes a grid in the Surfer binary format */
	unsigned int i, j;
	double x_max, y_max;
	float work_min = FLT_MAX, work_max = -FLT_MAX, tmp;
	struct srf_header h;
	FILE *fp;

	if ((fp = fopen (name, "wb")) == NULL) {
		mexPrintf("Fatal Error: Could not create file %s!\n", name);
		return (-1);
	}

	x_max = x_min + (i_end - i_start - 1) * x_inc;
	y_max = y_min + (j_end - j_start - 1) * y_inc;

	/* Find zmin/zmax */
	for (j = j_start; j < j_end; j++) {
		for (i = i_start; i < i_end; i++) {
			tmp = work[ijs(i,j,nX)];
			work_max = MAX(tmp, work_max);
			work_min = MIN(tmp, work_min);
		}
	}

	/* store header information and array */
	strcpy (h.id,"DSBB");
	h.nx = (i_end - i_start);	h.ny = (j_end - j_start);
	h.x_min = x_min;		h.x_max = x_max;
	h.y_min = y_min;		h.y_max = y_max;
	h.z_min = (double)work_min;	h.z_max = (double)work_max;

	if (fwrite ((void *)&h, sizeof (struct srf_header), (size_t)1, fp) != 1) {
		fprintf (stderr, "Fatal Error: Error writing file %s!\n", name);
		return (-1);
	}

	for (j = j_start; j < j_end; j++)
		for (i = i_start; i < i_end; i++)
			fwrite ((void *)&work[ijs(i,j,nX)], sizeof(float), (size_t)1, fp);

	fclose(fp);
	return (0);
}

/* ------------------------------------------------------------------------------ */
int read_grd_info_ascii(char *file, struct srf_header *hdr) {
	/* Read Surfer grid header, either in ASCII or binary */

	char line[128], id[5] = "";
	FILE *fp;

	if ((fp = fopen (file, "r")) == NULL) {
		mexPrintf ("NSWING: Unable to read file -- %s\n", file);
		return (-1);
	}

	fgets (line, 16, fp);
	sscanf (line, "%s", hdr->id);
	sprintf (id, "%.4s", hdr->id);
	if (strcmp (id, "DSAA") == 0) {
		fgets (line, 128, fp);
		sscanf (line, "%d %d", &hdr->nx, &hdr->ny);
		fgets (line, 128, fp);
		sscanf (line, "%lf %lf", &hdr->x_min, &hdr->x_max);
		fgets (line, 128, fp);
		sscanf (line, "%lf %lf", &hdr->y_min, &hdr->y_max);
		fgets (line, 128, fp);
		sscanf (line, "%lf %lf", &hdr->z_min, &hdr->z_max);
		fclose(fp);
		return (0);
	}
	else if (strcmp (id, "DSBB") == 0) {
		fclose(fp);
		fp = fopen (file, "rb");	/* Reopen in binary mode */
		read_header_bin (fp, hdr);
		fclose(fp);
		return (1);
	}
	else
		return (-1);
}

/* ------------------------------------------------------------------------------ */
int read_grd_ascii(char *file, struct srf_header *hdr, double *work, int sign) {
	/* sign is either +1 or -1, in case one wants to revert sign of the imported grid */

	/* Reads a grid in the Surfer ascii format */
	unsigned int i = 0, j, n_field;
	char *p, buffer[512], line[512];
	FILE *fp;

	if ((fp = fopen (file, "r")) == NULL) {
		mexPrintf ("NSWING: Unable to read file %s - exiting\n", file);
		return (-1);
	}

	fgets (line, 512, fp);
	sscanf (line, "%s", hdr->id);
	fgets (line, 512, fp);
	sscanf (line, "%d %d", &hdr->nx, &hdr->ny);
	fgets (line, 512, fp);
	sscanf (line, "%lf %lf", &hdr->x_min, &hdr->x_max);
	fgets (line, 512, fp);
	sscanf (line, "%lf %lf", &hdr->y_min, &hdr->y_max);
	fgets (line, 512, fp);
	sscanf (line, "%lf %lf", &hdr->z_min, &hdr->z_max);

	while (fgets (line, 512, fp) != NULL) {
		strcpy (buffer, line);
		n_field = count_col (buffer);	/* Count # of fields in line */
		if (n_field == 0) continue;
		p = (char *)strtok (line, " \t\n\015\032");
		j = 0;
		while (p && j < n_field) {
			sscanf (p, "%lf", &work[i]);
			work[i] *= sign;
			j++;	i++;
			p = (char *)strtok ((char *)NULL, " \t\n\015\032");
		}
	}
	fclose(fp);
	return (0);
} 

/* -------------------------------------------------------------------- */
int count_col(char *line) {
	/* Count # of fields contained in line */
	int   n_col = 0;
	char *p, *ntoken = NULL;

	p = (char *)strtok_s (line, " \t\n\015\032", &ntoken);
	while (p) {	/* Count # of fields */
		n_col++;
		p = (char *)strtok_s (NULL, " \t\n\015\032", &ntoken);
	}
	return (n_col);
}

/* -------------------------------------------------------------------- */
int read_header_bin(FILE *fp, struct srf_header *hdr) {
	/* Reads the header of a binary Surfer gridfile */
	fread ((void *)hdr, sizeof (struct srf_header), (size_t)1, fp); 
	return (0);
}

/* -------------------------------------------------------------------- */
int read_grd_bin(char *file, struct srf_header *hdr, double *work, int sign) {
	/* sign is either +1 or -1, in case one wants to revert sign of the imported grid */
	int i, j;
	unsigned int ij, kk;
	float	*tmp;			/* Array pointer for reading in rows of data */
	FILE	*fp;

	if ((fp = fopen (file, "rb")) == NULL) {
		mexPrintf ("NSWING: Unable to read file %s - exiting\n", file);
		return (-1);
	}
	fread ((void *)hdr, sizeof (struct srf_header), (size_t)1, fp); 

	/* Allocate memory for one row of data (for reading purposes) */
	tmp = (float *) mxMalloc ((size_t)hdr->nx * sizeof (float));
	for (j = 0; j < hdr->ny; j++) {
		fread (tmp, sizeof (float), (size_t)hdr->nx, fp);	/* Get one row */
		ij = j * hdr->nx;
		for (i = 0; i < hdr->nx; i++) {
			kk = ij + i;
			work[kk] = tmp[i] * sign;
		}
	}
	fclose(fp);
	mxFree ((void *)tmp);

	return (0);
}

/* -------------------------------------------------------------------- */
int count_n_maregs(char *file) {
	int     i = 0;
	char    line[256];
	FILE   *fp;

	if ((fp = fopen (file, "r")) == NULL) {
		mexPrintf ("NSWING: Unable to open file %s - exiting\n", file);
		return (-1);
	}
	while (fgets (line, 256, fp) != NULL) {
		if (line[0] == '#') continue;	/* Jump comment lines */
		i++;
	}
	fclose (fp);
	return(i);
}

/* -------------------------------------------------------------------- */
int read_maregs(struct grd_header hdr, char *file, unsigned int *lcum_p, char *names[]) {
	/* Read maregraph positions and convert them to vector linear indices */
	int     i = 0, k = 0, ix, jy, n;
	char    line[256], txt[64];
	double  x, y;
	FILE   *fp;

	if ((fp = fopen (file, "r")) == NULL) {
		mexPrintf ("NSWING: Unable to open file %s - exiting\n", file);
		return (-1);
	}

	while (fgets (line, 256, fp) != NULL) {
		k++;
		if (line[0] == '#') continue;	/* Jump comment lines */
		n = sscanf (line, "%lf %lf %s", &x, &y, txt);
		if (n !=2 && n != 3) {
			if (n == 1) {				/* Try with commas */
				n = sscanf (line, "%lf %lf %s", &x, &y, txt);
				if (n !=2 && n != 3) {
					mexPrintf("NSWING: Error reading maregraph file at line %d Expected 2 or 3 values but got %d\n", k, n);
					continue;
				}
			}
		}

		if (x < hdr.x_min || x > hdr.x_max || y < hdr.y_min || y > hdr.y_max)
			continue;
		ix = irint((x - hdr.x_min) / hdr.x_inc);
		jy = irint((y - hdr.y_min) / hdr.y_inc);
		lcum_p[i] = jy * hdr.nx + ix; 

		if (n == 3)		/* This maregraph's name */
			names[i] = strdup(&txt[0]);
		else
			names[i] = strdup("NoName");
		i++;
	}
	fclose (fp);
	return (i);
}

/* -------------------------------------------------------------------- */
int read_tracers(struct grd_header hdr, char *file, struct tracers *oranges) {
	/* Read tracers positions */
	int     i = 0, k = 0, ix, jy, n;
	char    line[256];
	double  x, y;
	FILE   *fp;

	if ((fp = fopen (file, "r")) == NULL) {
		mexPrintf ("NSWING: Unable to open file %s - exiting\n", file);
		return (-1);
	}

	while (fgets (line, 256, fp) != NULL) {
		k++;
		if (line[0] == '#') continue;	/* Jump comment lines */
		if ((n = sscanf (line, "%lf %lf", &x, &y)) != 2) {
			if (n == 1) {				/* Try with commas */
				if ((n = sscanf(line, "%lf,%lf", &x, &y)) != 2) {
					mexPrintf("NSWING: Error reading maregraph file at line %d Expected 2 values but got %d\n", k, n);
					continue;
				}
			}
		}
		if (x < hdr.x_min || x > hdr.x_max || y < hdr.y_min || y > hdr.y_max)
			continue;

		oranges[i].x[0] = x;
		oranges[i].y[0] = y;
		i++;
	}
	fclose (fp);
	return (i);
}

/* -------------------------------------------------------------------- */
int read_bnc_file(struct nestContainer *nest, char *file) {
	/* Read file with a boundary condition time series */
	int	N, n, n_ts = 0, n_pts, n_vars, done_nPts = FALSE, first_vars = TRUE, foundB = FALSE, n_alloc = 2048;
	char	*p, line[256], buffer[256], *ntoken = NULL;
	FILE	*fp;

	if ((fp = fopen(file, "r")) == NULL) {
		mexPrintf("NSWING: Unable to open file %s - exiting\n", file);
		return (-1);
	}

	while ((fgets(line, 256, fp) != NULL) && line[0] == '#') {
		if ((p = strstr(line, "B:S")) != NULL) {        /* South */
			nest->bnc_border[1] = TRUE;
			foundB = TRUE;
			break;
		}
		else if ((p = strstr(line, "B:W")) != NULL) {   /* West */
			nest->bnc_border[0] = TRUE;
			foundB = TRUE;
			break;
		}
		else if ((p = strstr(line, "B:E")) != NULL) {   /* West */
			nest->bnc_border[2] = TRUE;
			foundB = TRUE;
			break;
		}
		else if ((p = strstr(line, "B:N")) != NULL) {   /* West */
			nest->bnc_border[3] = TRUE;
			foundB = TRUE;
			break;
		}
	}
	if (!foundB) {
		nest->bnc_border[1] = TRUE;	/* <<<<<<<<<<<<<<< TEMP ---------- */
		fprintf(stderr, "\n\n\tATENCAO E PRECISO ESPECIFICAR A FRONTEIRA NO FICHE DA ONDA (ex: # B:S)\n");
		fprintf(stderr, "\tDAQUI A ALGUM TEMPO NAO O FAZER DARA UM ERRO\n\n");
	}

	while (fgets(line, 256, fp) != NULL) {
		if (line[0] == '#') continue;	/* Jump comment lines */
		if (!done_nPts) {
			strcpy(buffer, line);
			n_pts = count_col(buffer);	/* Count # of points along border */
			if (n_pts % 2 || n_pts < 2) {
				mexPrintf ("NSWING: %s Must have at least one pair of (x,y) points\n", file);
				return (-1);
			}
			strcpy(buffer, line);		/* Copy it again because buffer was 'consumed' in count_col() */
			n_pts /= 2;	/* It was the number of x and y */
			nest->bnc_pos_x = (double *) mxMalloc((size_t)n_pts * sizeof(double));
			nest->bnc_pos_y = (double *) mxMalloc((size_t)n_pts * sizeof(double));
			p = (char *)strtok_s(buffer, " \t\n\015\032", &ntoken);
			sscanf (p, "%lf", &nest->bnc_pos_x[0]);
			p = (char *)strtok_s(NULL, " \t\n\015\032", &ntoken);
			sscanf (p, "%lf", &nest->bnc_pos_y[0]);
			for (n = 1; n < n_pts; n++) {		/* Now read remaining points */
				p = (char *)strtok_s(NULL, " \t\n\015\032", &ntoken);
				sscanf (p, "%lf", &nest->bnc_pos_x[n]);
				p = (char *)strtok_s(NULL, " \t\n\015\032", &ntoken);
				sscanf (p, "%lf", &nest->bnc_pos_y[n]);
			}
			done_nPts = TRUE;
			nest->bnc_pos_nPts = n_pts;
		}

		strcpy(buffer, line);		/* 'line' is now a data row */
		n_vars = count_col(buffer);	/* Count # of variables. */
		if (n_vars == 0) continue;	/* Empty line */
		if (n_vars < 2) {
			mexPrintf ("NSWING: Variables on bnc file (%s) must be (t,z1,[z2,...]), but got only %d fields\n", file, n_vars);
			return (-1);
		}
		if (first_vars) {
			N = n_vars;
			nest->bnc_var_t    = (double *)  mxMalloc((size_t)n_alloc * sizeof(double));
			nest->bnc_var_z    = (double **) mxMalloc((size_t)n_alloc * sizeof(double));
			nest->bnc_var_zTmp = (double *)  mxMalloc((size_t)(N-1)   * sizeof(double));
			for (n = 0; n < n_alloc; n++)
				nest->bnc_var_z[n] = (double *) mxMalloc((size_t)(N-1) * sizeof(double));

			/*if (N == 4) {
				nest->bnc_var_U = (double *) mxMalloc ((size_t)n_alloc * sizeof(double));
				nest->bnc_var_V = (double *) mxMalloc ((size_t)n_alloc * sizeof(double));
			}*/
			first_vars = FALSE;
		}
		if (!first_vars && N != n_vars) {
			mexPrintf ("NSWING: WARNING, expected %d variables but found %d Ignoring this entry\n", N, n_vars);
			continue;
		}
		p = (char *)strtok_s(line, " \t\n\015\032", &ntoken);
		sscanf (p, "%lf", &nest->bnc_var_t[n_ts]);
		for (n = 0; n < N-1; n++) {
			p = (char *)strtok_s(NULL, " \t\n\015\032", &ntoken);
			sscanf (p, "%lf", &nest->bnc_var_z[n_ts][n]);
		}
		//else 
			//sscanf (line, "%lf %lf %lf %lf", &nest->bnc_var_t[n_ts], &nest->bnc_var_z[n_ts],
			//&nest->bnc_var_U[n_ts], &nest->bnc_var_V[n_ts]);

		n_ts++;
		if (n_ts == n_alloc) {
			n_alloc += 2048;
			nest->bnc_var_t = (double *)  mxRealloc(nest->bnc_var_t, (size_t)n_alloc * sizeof(double));
			nest->bnc_var_z = (double **) mxRealloc(nest->bnc_var_z, (size_t)n_alloc * sizeof(double));
			for (n = n_ts; n < n_alloc; n++)
				nest->bnc_var_z[n] = (double *)mxMalloc((size_t)(N - 1) * sizeof(double));
			/*if (N == 4) {
				nest->bnc_var_U = (double *) mxRealloc(nest->bnc_var_U, (size_t)n_alloc * sizeof(double));
				nest->bnc_var_V = (double *) mxRealloc(nest->bnc_var_V, (size_t)n_alloc * sizeof(double));
			}*/
		}
	}
	fclose (fp);
	nest->bnc_var_nTimes = n_ts;
	return (0);
}

/* -------------------------------------------------------------------- */
int interp_bnc(struct nestContainer *nest, double t) {
	int i, n, side_len;
	double s;

	/* Pick up the appropriate dimension for this bnc interpolation */
	if ((nest->bnc_border[0] != 0) || (nest->bnc_border[2] != 0))	/* West or East borders */ 
		side_len = nest->hdr[0].ny;
	else
		side_len = nest->hdr[0].nx;

	for (n = 0; n < nest->bnc_var_nTimes - 1; n++) {		/* Interp boundary data, normally with cruder dt, for the model run time dt */
		if (t >= nest->bnc_var_t[n] && t < nest->bnc_var_t[n+1]) {		/* Found the bounds of 't' */
			s = (t - nest->bnc_var_t[n]) / (nest->bnc_var_t[n+1] - nest->bnc_var_t[n]);
			for (i = 0; i < nest->bnc_pos_nPts; i++)		/* Interpolate all spatial points for time t */
				nest->bnc_var_zTmp[i] = nest->bnc_var_z[n][i] + s * (nest->bnc_var_z[n+1][i] - nest->bnc_var_z[n][i]);

			break;
		}
		else if (t > nest->bnc_var_nTimes) {		/* Once this is TRUE this function wont be called anymore */
			return TRUE;
		}
	}

	if (nest->bnc_pos_nPts == 1) {		/* One point only, replicate it into all values of the line */
		for (n = 0; n < side_len; n++)
			nest->bnc_var_z_interp[n] = nest->bnc_var_zTmp[0];
	}
	else { 		/* This case actually not yet implemented via the bnc file (-B option) */
		if (side_len == nest->hdr[0].nx)
			intp_lin(nest->bnc_pos_x, nest->bnc_var_zTmp, nest->bnc_pos_nPts, side_len,
			         nest->edge_row_P[0], nest->bnc_var_z_interp);
		else
			intp_lin(nest->bnc_pos_y, nest->bnc_var_zTmp, nest->bnc_pos_nPts, side_len,
			         nest->edge_row_P[0], nest->bnc_var_z_interp);
	}
	return FALSE;
}

/* -------------------------------------------------------------------- */
void no_sys_mem(char *where, unsigned int n) {	
		mexPrintf("Fatal Error: %s could not allocate memory, n = %d\n", where, n);
#ifdef I_AM_MEX
		mexErrMsgTxt("\n");
#endif
}

/* -------------------------------------------------------------------- */
int decode_R(char *item, double *w, double *e, double *s, double *n) {
	char *text, string[BUFSIZ];
	
	/* Minimalist code to decode option -R extracted from GMT_get_common_args */
	
	int i, error = 0;
	double *p[4];
	
	p[0] = w;	p[1] = e;	p[2] = s;	p[3] = n;
			
	i = 0;
	strcpy(string, &item[2]);
	text = strtok (string, "/");
	while (text) {
		*p[i] = ddmmss_to_degree(text);
		i++;
		text = strtok (CNULL, "/");
	}
	if (item[strlen(item)-1] == 'r')	/* Rectangular box given, but valid here */
		error++;
	if (i != 4 || check_region(*p[0], *p[1], *p[2], *p[3]))
		error++;
	w = p[0];	e = p[1];
	s = p[2];	n = p[3];
	return (error);
}

/* -------------------------------------------------------------------- */
int check_region(double w, double e, double s, double n) {
	/* If region is given then we must have w < e and s < n */
	return ((w >= e || s >= n));
}

/* -------------------------------------------------------------------- */
double ddmmss_to_degree(char *text) {
	int i, colons = 0, suffix;
	double degree, minute, degfrac, second;

	for (i = 0; text[i]; i++) if (text[i] == ':') colons++;
	suffix = (int)text[i-1];	/* Last character in string */
	if (colons == 2) {	/* dd:mm:ss format */
		sscanf (text, "%lf:%lf:%lf", &degree, &minute, &second);
		degfrac = degree + Loc_copysign (minute / 60.0 + second / 3600.0, degree);
	}
	else if (colons == 1) {	/* dd:mm format */
		sscanf (text, "%lf:%lf", &degree, &minute);
		degfrac = degree + Loc_copysign (minute / 60.0, degree);
	}
	else
		degfrac = atof (text);
	if (suffix == 'W' || suffix == 'w' || suffix == 'S' || suffix == 's') degfrac = -degfrac;	/* Sign was given implicitly */
	return (degfrac);
}

#ifdef HAVE_NETCDF
/* -------------------------------------------------------------------- */
int open_most_nc(struct nestContainer *nest, float *work, char *base, char *name_var, char hist[], int *ids,
	unsigned int nx, unsigned int ny, double xMinOut, double yMinOut, int isMost, int lev) {
	/* Open and initialize a generic 3D or a MOST netCDF file for writing.
	   When generic 3D file also writes the bathymetry grid right away.
	   Returns the ncid of the opened file.
	*/
	char    *long_name = NULL, *units = NULL, *basename = NULL;
	int      ncid = -1, status, dim0[3], dim3[3], id;
	unsigned int m, n, ij;
	float    dummy = -1e34f;
	double  *x, *y;

	basename = (char *)mxMalloc(strlen(base) * sizeof(char));
	strcpy(basename, base);
	if (!strcmp(name_var,"HA")) {
		strcat(basename,"_ha.nc");
		long_name = "Wave Amplitude";
		units = "CENTIMETERS";
	}
	else if (!strcmp(name_var,"VA")) {
		strcat(basename,"_va.nc");
		long_name = "Velocity Component along Latitude";
		units = "CENTIMETERS/SECOND";
	}
	else if (!strcmp(name_var,"UA")) {
		strcat(basename,"_ua.nc");
		long_name = "Velocity Component along Longitude";
		units = "CENTIMETERS/SECOND";
	}
	else if (!strcmp(name_var,"z")) {	/* Generic variable in meters */
		long_name = "Sea surface";
		units = "meters";
	}

	if ((status = nc_create(basename, NC_NETCDF4, &ncid)) != NC_NOERR) {
		mexPrintf ("NSWING: Unable to create file %s - exiting\n", basename);
		return(-1);
	}

	if (nest->isGeog) {
		/* ---- Define dimensions ------------ */
		err_trap(nc_def_dim(ncid, "LON", (size_t) nx,   &dim0[0]));
		err_trap(nc_def_dim(ncid, "LAT", (size_t) ny,   &dim0[1]));
		err_trap(nc_def_dim(ncid, "time", NC_UNLIMITED, &dim0[2]));

		/* ---- Define variables ------------- */
		dim3[0] = dim0[2];	dim3[1] = dim0[1];	dim3[2] = dim0[0];
		err_trap(nc_def_var(ncid, "LON",       NC_DOUBLE,1, &dim0[0], &ids[0]));
		err_trap(nc_def_var(ncid, "LAT",       NC_DOUBLE,1, &dim0[1], &ids[1]));
		if (isMost) {
			err_trap(nc_def_var(ncid, "SLON",  NC_FLOAT, 0, &dim0[0], &ids[2]));
			err_trap(nc_def_var(ncid, "SLAT",  NC_FLOAT, 0, &dim0[1], &ids[3]));
			err_trap(nc_def_var(ncid, "time",  NC_DOUBLE,1, &dim0[2], &ids[4]));
			err_trap(nc_def_var(ncid, name_var,NC_FLOAT, 3, dim3,     &ids[5]));
		}
		else {
			err_trap(nc_def_var(ncid, "time",   NC_DOUBLE,1, &dim0[2], &ids[2]));
			err_trap(nc_def_var(ncid, name_var, NC_FLOAT, 3, dim3,     &ids[3]));
			if (nest->out_momentum) {
				err_trap(nc_def_var(ncid, "Mlon", NC_FLOAT,3, dim3,  &ids[5]));
				err_trap(nc_def_var(ncid, "Mlat", NC_FLOAT,3, dim3,  &ids[6]));
			}
			if (nest->out_velocity_x)
				err_trap(nc_def_var(ncid, "Vlon", NC_FLOAT,3, dim3,  &ids[5]));
			if (nest->out_velocity_y)
				err_trap(nc_def_var(ncid, "Vlat", NC_FLOAT,3, dim3,  &ids[6]));
			dim3[0] = dim0[1];			dim3[1] = dim0[0];		/* Bathym array is rank 2 */
			err_trap(nc_def_var(ncid, "bathymetry",NC_FLOAT,2, dim3,  &ids[4]));
		}
	}
	else {		/* Cartesian */
		err_trap(nc_def_dim(ncid, "x",    (size_t)nx,   &dim0[0]));
		err_trap(nc_def_dim(ncid, "y",    (size_t)ny,   &dim0[1]));
		err_trap(nc_def_dim(ncid, "time", NC_UNLIMITED, &dim0[2]));

		dim3[0] = dim0[2];	dim3[1] = dim0[1];   dim3[2] = dim0[0];
		err_trap(nc_def_var(ncid, "x",         NC_DOUBLE,1, &dim0[0], &ids[0]));
		err_trap(nc_def_var(ncid, "y",         NC_DOUBLE,1, &dim0[1], &ids[1]));
		if (isMost) {
			err_trap(nc_def_var(ncid, "SLON",  NC_FLOAT,0,  &dim0[0], &ids[2]));
			err_trap(nc_def_var(ncid, "SLAT",  NC_FLOAT,0,  &dim0[1], &ids[3]));
			err_trap(nc_def_var(ncid, "time",  NC_DOUBLE,1, &dim0[2], &ids[4]));
			err_trap(nc_def_var(ncid, name_var,NC_FLOAT,3,  dim3,     &ids[5]));
		}
		else {
			err_trap(nc_def_var(ncid, "time",  NC_DOUBLE,1, &dim0[2], &ids[2]));
			err_trap(nc_def_var(ncid, name_var,NC_FLOAT,3,  dim3,     &ids[3]));
			if (nest->out_momentum) {
				err_trap(nc_def_var(ncid, "Mx",NC_FLOAT,3, dim3,  &ids[5]));
				err_trap(nc_def_var(ncid, "My",NC_FLOAT,3, dim3,  &ids[6]));
			}
			if (nest->out_velocity_x)
				err_trap(nc_def_var(ncid, "Vx",NC_FLOAT,3, dim3,  &ids[5]));
			if (nest->out_velocity_y)
				err_trap(nc_def_var(ncid, "Vy",NC_FLOAT,3, dim3,  &ids[6]));
			dim3[0] = dim0[1];			dim3[1] = dim0[0];		/* Bathym array is rank 2 */
			err_trap(nc_def_var(ncid, "bathymetry",NC_FLOAT,2, dim3, &ids[4]));
		}
	}

	if (!isMost) {
		if (nest->do_long_beach)
			err_trap(nc_def_var(ncid, "LongBeach",  NC_UBYTE, 2, dim3, &ids[7]));
		if (nest->do_short_beach)
			err_trap(nc_def_var(ncid, "ShortBeach", NC_UBYTE, 2, dim3, &ids[8]));
	}

	/* Set a deflation level of 5 (4 zero based) and shuffle for z variable */
	id = (isMost) ? 5 : 3;
	err_trap(nc_def_var_deflate(ncid, ids[id], 1, 1, 4));

	/* ---- Variables Attributes --------- */
	if (isMost) {
		err_trap(nc_put_att_text (ncid, ids[0], "units", 12, "degrees_east"));
		err_trap(nc_put_att_text (ncid, ids[0], "point_spacing", 4, "even"));
		err_trap(nc_put_att_text (ncid, ids[1], "units", 13, "degrees_north"));
		err_trap(nc_put_att_text (ncid, ids[1], "point_spacing", 4, "even"));
		err_trap(nc_put_att_text (ncid, ids[2], "units", 12, "degrees_east"));
		err_trap(nc_put_att_text (ncid, ids[2], "long_name", 16, "Source Longitude"));
		err_trap(nc_put_att_text (ncid, ids[3], "units", 13, "degrees_north"));
		err_trap(nc_put_att_text (ncid, ids[3], "long_name", 16, "Source Latitude"));
		err_trap(nc_put_att_text (ncid, ids[4], "units", 7, "SECONDS"));
		err_trap(nc_put_att_text (ncid, ids[5], "long_name", strlen(long_name), long_name));
		err_trap(nc_put_att_text (ncid, ids[5], "units", strlen(units), units));
		err_trap(nc_put_att_float(ncid, ids[5], "missing_value", NC_FLOAT, 1, &dummy));
		err_trap(nc_put_att_float(ncid, ids[5], "_FillValue", NC_FLOAT, 1, &dummy));
		err_trap(nc_put_att_text (ncid, ids[5], "history", 6, "Nikles"));
	}
	else {
		size_t	start_b[2] = {0,0}, count_b[2];
		double dummy[2] = {0, 0}, range[2];
		float nan = (float)mxGetNaN();

		if (nan == 0) nan = (float)loc_nan.d;	/* Dirty hack. With the Intel compiler for example mxGetNaN() returns 0*/

		range[0] = xMinOut;		range[1] = xMinOut + (nx - 1) * nest->hdr[lev].x_inc;
		err_trap(nc_put_att_double(ncid, ids[0], "actual_range", NC_DOUBLE, 2U, range));
		range[0] = yMinOut;		range[1] = yMinOut + (ny - 1) * nest->hdr[lev].y_inc;
		err_trap(nc_put_att_double(ncid, ids[1], "actual_range", NC_DOUBLE, 2U, range));
		if (nest->isGeog) {
			err_trap(nc_put_att_text(ncid, ids[0], "units", 12, "degrees_east"));
			err_trap(nc_put_att_text(ncid, ids[1], "units", 13, "degrees_north"));
		}
		else {
			err_trap(nc_put_att_text(ncid, ids[0], "units", 6, "meters"));
			err_trap(nc_put_att_text(ncid, ids[1], "units", 6, "meters"));
		}
		err_trap(nc_put_att_text  (ncid, ids[2], "units", 7, "Seconds"));
		err_trap(nc_put_att_text  (ncid, ids[3], "long_name", strlen(long_name), long_name));
		err_trap(nc_put_att_text  (ncid, ids[3], "units", strlen(units), units));
		err_trap(nc_put_att_float (ncid, ids[3], "missing_value", NC_FLOAT, 1, &nan));
		err_trap(nc_put_att_float (ncid, ids[3], "_FillValue", NC_FLOAT, 1, &nan));
		err_trap(nc_put_att_double(ncid, ids[3], "actual_range", NC_DOUBLE, 2U, dummy));

		err_trap(nc_put_att_text  (ncid, ids[4], "long_name", 10, "bathymetry"));
		err_trap(nc_put_att_text  (ncid, ids[4], "units", 6, "meters"));
		err_trap(nc_put_att_float (ncid, ids[4], "missing_value", NC_FLOAT, 1, &nan));
		err_trap(nc_put_att_float (ncid, ids[4], "_FillValue", NC_FLOAT, 1, &nan));
		range[0] = nest->hdr[lev].z_min;	range[1] = nest->hdr[lev].z_max;
		err_trap(nc_put_att_double(ncid, ids[4], "actual_range", NC_DOUBLE, 2U, range));

		for (ij = 0; ij < nest->hdr[lev].nm; ij++)		/* Change bathy sign back to pos up and copy to work array */
			work[ij] = (float)-nest->bat[lev][ij];

		count_b[0] = nest->hdr[lev].ny;	count_b[1] = nest->hdr[lev].nx;
		err_trap(nc_put_vara_float(ncid, ids[4], start_b, count_b, work));	/* Write the bathymetry */

		if (nest->out_momentum) {
			long_name = "Moment Component along x/Longitude";
			err_trap(nc_put_att_text  (ncid, ids[5], "long_name", strlen(long_name), long_name));
			err_trap(nc_put_att_text  (ncid, ids[5], "units", 15, "Meters^2/second"));
			err_trap(nc_put_att_float (ncid, ids[5], "missing_value", NC_FLOAT, 1, &nan));
			err_trap(nc_put_att_float (ncid, ids[5], "_FillValue", NC_FLOAT, 1, &nan));
			err_trap(nc_put_att_double(ncid, ids[5], "actual_range", NC_DOUBLE, 2U, dummy));
			long_name = "Moment Component along x/Latitude";
			err_trap(nc_put_att_text  (ncid, ids[6], "long_name", strlen(long_name), long_name));
			err_trap(nc_put_att_text  (ncid, ids[6], "units", 15, "Meters^2/second"));
			err_trap(nc_put_att_float (ncid, ids[6], "missing_value", NC_FLOAT, 1, &nan));
			err_trap(nc_put_att_float (ncid, ids[6], "_FillValue", NC_FLOAT, 1, &nan));
			err_trap(nc_put_att_double(ncid, ids[6], "actual_range", NC_DOUBLE, 2U, dummy));
		}
		if (nest->out_velocity_x) {			/* Horizontal velocity, 3D case */
			long_name = "Velocity Component along x/Longitude";
			err_trap(nc_put_att_text  (ncid, ids[5], "long_name", strlen(long_name), long_name));
			err_trap(nc_put_att_text  (ncid, ids[5], "units", 13, "Meters/second"));
			err_trap(nc_put_att_float (ncid, ids[5], "missing_value", NC_FLOAT, 1, &nan));
			err_trap(nc_put_att_float (ncid, ids[5], "_FillValue", NC_FLOAT, 1, &nan));
			err_trap(nc_put_att_double(ncid, ids[5], "actual_range", NC_DOUBLE, 2U, dummy));
		}
		if (nest->out_velocity_y) {			/* Vertical velocity, 3D case */
			long_name = "Velocity Component along x/Latitude";
			err_trap(nc_put_att_text  (ncid, ids[6], "long_name", strlen(long_name), long_name));
			err_trap(nc_put_att_text  (ncid, ids[6], "units", 13, "Meters/second"));
			err_trap(nc_put_att_float (ncid, ids[6], "missing_value", NC_FLOAT, 1, &nan));
			err_trap(nc_put_att_float (ncid, ids[6], "_FillValue", NC_FLOAT, 1, &nan));
			err_trap(nc_put_att_double(ncid, ids[6], "actual_range", NC_DOUBLE, 2U, dummy));
		}

		if (nest->do_long_beach) {
			float act_range[2] = {0, 0};
			long_name = "Mask of receded water";
			err_trap(nc_put_att_text  (ncid, ids[7], "long_name", strlen(long_name), long_name));
			err_trap(nc_put_att_text  (ncid, ids[7], "units", 3, "0/1"));
			err_trap(nc_put_att_float (ncid, ids[7], "actual_range", NC_FLOAT, 2U, act_range));
		}
		if (nest->do_short_beach) {
			float act_range[2] = {0, 0};
			long_name = "Mask of innundation";
			err_trap(nc_put_att_text  (ncid, ids[8], "long_name", strlen(long_name), long_name));
			err_trap(nc_put_att_text  (ncid, ids[8], "units", 3, "0/1"));
			err_trap(nc_put_att_float(ncid,  ids[8], "actual_range", NC_FLOAT, 2U, act_range));
		}
	}

	/* ---- Global Attributes ------------ */
	err_trap(nc_put_att_text(ncid, NC_GLOBAL, "Conventions",   13, "COARDS/CF-1.0"));
	err_trap(nc_put_att_text(ncid, NC_GLOBAL, "history",       10, "Mirone Tec"));
	if (isMost) {
		err_trap(nc_put_att_text(ncid, NC_GLOBAL, "title", 39, "MOST type file created by Mirone-NSWING"));
	}
	else {
		err_trap(nc_put_att_text(ncid, NC_GLOBAL, "title", 44, "Water levels series created by Mirone-NSWING"));
		err_trap(nc_put_att_text(ncid, NC_GLOBAL, "TSU",    6, "NSWING"));
	}
	err_trap(nc_put_att_text(ncid, NC_GLOBAL, "History", strlen(hist), hist));

	err_trap(nc_enddef(ncid));

	/* ---- Write the vector coords ------ */
	x = (double *)mxMalloc (sizeof(double) * nx);
	y = (double *)mxMalloc (sizeof(double) * ny);

	for (n = 0; n < nx; n++) x[n] = xMinOut + n * nest->hdr[lev].x_inc;
	for (m = 0; m < ny; m++) y[m] = yMinOut + m * nest->hdr[lev].y_inc;
	err_trap(nc_put_var_double(ncid, ids[0], x));
	err_trap(nc_put_var_double(ncid, ids[1], y));
	mxFree((void *)x); 
	mxFree((void *)y); 
	mxFree((void *)basename); 

	return (ncid);
}

/* --------------------------------------------------------------------------- */
void write_most_slice(struct nestContainer *nest, int *ncid, int *ids, unsigned int i_start, unsigned int j_start,
                      unsigned int i_end, unsigned int j_end, float *work, size_t *start, size_t *count,
                      double *slice_range, int isMost, int lev) {
	/* Write a slice of _ha.nc, _va.nc & _ua.nc MOST netCDF files */
	unsigned int col, row, n, ij, k;

	if (!isMost) {
		/* ----------- Find the min/max of this slice --------- */
		for (ij = 0; ij < nest->hdr[lev].nm; ij++) {
			slice_range[0] = MIN(work[ij], slice_range[0]);
			slice_range[1] = MAX(work[ij], slice_range[1]);
		}

		err_trap(nc_put_vara_float(ncid[0], ids[0], start, count, work));

		/* Conditionally write the Vx & Vy velocity components */
		if (nest->out_velocity_x) {
			for (ij = 0; ij < nest->hdr[nest->writeLevel].nm; ij++) {
				work[ij] = (nest->htotal_d[nest->writeLevel][ij] > EPS2) ? (float)nest->vex[nest->writeLevel][ij] : 0;
				if (nest->htotal_d[nest->writeLevel][ij] < 0.5 && fabs(work[ij]) >= V_LIMIT)	/* Clip above this combination */
					work[ij] = 0;

				slice_range[2] = MIN(work[ij], slice_range[2]);
				slice_range[3] = MAX(work[ij], slice_range[3]);
			}			
			err_trap(nc_put_vara_float(ncid[0], ids[1], start, count, work));
		}
		if (nest->out_velocity_y) {
			for (ij = 0; ij < nest->hdr[nest->writeLevel].nm; ij++) {
				work[ij] = (nest->htotal_d[nest->writeLevel][ij] > EPS2) ? (float)nest->vey[nest->writeLevel][ij] : 0;
				if (nest->htotal_d[nest->writeLevel][ij] < 0.5 && fabs(work[ij]) >= V_LIMIT)	/* Clip above this combination */
					work[ij] = 0;

				slice_range[4] = MIN(work[ij], slice_range[4]);
				slice_range[5] = MAX(work[ij], slice_range[5]);
			}			
			err_trap (nc_put_vara_float (ncid[0], ids[2], start, count, work));
		}
		if (nest->out_momentum) {
			for (ij = 0; ij < nest->hdr[nest->writeLevel].nm; ij++) {
				work[ij] = (float)nest->fluxm_d[nest->writeLevel][ij];
				slice_range[2] = MIN(work[ij], slice_range[2]);
				slice_range[3] = MAX(work[ij], slice_range[3]);
			}
			err_trap (nc_put_vara_float (ncid[0], ids[1], start, count, work));
			for (ij = 0; ij < nest->hdr[nest->writeLevel].nm; ij++) {
				work[ij] = (float)nest->fluxn_d[nest->writeLevel][ij];
				slice_range[4] = MIN(work[ij], slice_range[2]);
				slice_range[5] = MAX(work[ij], slice_range[3]);
			}
			err_trap (nc_put_vara_float (ncid[0], ids[2], start, count, work));
		}
	}
	else {
		for (n = 0; n < 3; n++) {	/* Loop over, Amplitude, Xmomentum & Ymomentum */
			if (n == 0) {		/* Amplitude */
				for (row = j_start, k = 0; row < j_end; row++)
					for (col = i_start; col < i_end; col++)
						work[k++] = (float)(nest->etad[lev][ij_grd(col, row, nest->hdr[lev])] * 100);

				err_trap (nc_put_vara_float (ncid[0], ids[0], start, count, work));
			}
			else if (n == 1) {		/* X velocity */ 
				for (row = j_start, k = 0; row < j_end; row++) {
					for (col = i_start; col < i_end; col++) {
						ij = ij_grd(col, row, nest->hdr[lev]);
						work[k++] = (nest->htotal_d[lev][ij] < EPS3) ? 0 :
						            (float)(nest->fluxm_d[lev][ij] / nest->htotal_d[lev][ij] * 100);
					}
				}
				err_trap (nc_put_vara_float (ncid[1], ids[1], start, count, work));
			}
			else {				/* Y velocity */ 
				for (row = j_start, k = 0; row < j_end; row++) {
					for (col = i_start; col < i_end; col++) {
						ij = ij_grd(col, row, nest->hdr[lev]);
						work[k++] = (nest->htotal_d[lev][ij] < EPS3) ? 0 :
						            (float)(nest->fluxn_d[lev][ij] / nest->htotal_d[lev][ij] * 100);
					}
				}
				err_trap (nc_put_vara_float (ncid[2], ids[2], start, count, work));
			}
		}
	}
}

/* -------------------------------------------------------------------- */
int write_greens_nc(struct nestContainer *nest, char *fname, float *work, size_t *start, size_t *count,
	double *t, unsigned int *lcum_p, char *names[], char hist[], int *ids, int n_maregs,
	unsigned int n_times, int lev) {
	/* Save the maregraphs time series in a netCDF file. This version is for the KabaGrid case.
	   t        - is the vector of times
	   work     - array of maregraphs with one per column
	   n_maregs - number of maregraphs
	   n_times  - length(t)
	*/

	int     k, ix, iy, ncid = -1, status, dim0[4], dim2[2], dim3[2];
	double *x, *y;

	if ((status = nc_create(fname, NC_NETCDF4, &ncid)) != NC_NOERR) {
		mexPrintf("NSWING: Unable to create file -- %s -- exiting\n", fname);
		return(-1);
	}

	/* ---- Define dimensions ------------ */
	err_trap(nc_def_dim(ncid, "countMareg",  (size_t)n_maregs, &dim0[0]));
	err_trap(nc_def_dim(ncid, "time",        (size_t)n_times,  &dim0[1]));
	err_trap(nc_def_dim(ncid, "TM",          (size_t)n_times*n_maregs,  &dim0[2]));
	err_trap(nc_def_dim(ncid, "binIndex",     NC_UNLIMITED,    &dim0[3]));

	/* ---- Define variables ------------- */
	dim2[0] = dim0[3];   dim2[1] = dim0[2];
	dim3[0] = dim0[3];   dim3[1] = dim0[2];
	err_trap(nc_def_var(ncid, "time",         NC_DOUBLE,1, &dim0[1], &ids[0]));
	if (nest->isGeog) {
		err_trap(nc_def_var(ncid, "lonMareg", NC_DOUBLE,1, &dim0[0], &ids[1]));
		err_trap(nc_def_var(ncid, "latMareg", NC_DOUBLE,1, &dim0[0], &ids[2]));
	}
	else {
		err_trap(nc_def_var(ncid, "xMareg",   NC_DOUBLE,1, &dim0[0], &ids[1]));
		err_trap(nc_def_var(ncid, "yMareg",   NC_DOUBLE,1, &dim0[0], &ids[2]));
	}
	err_trap(nc_def_var(ncid, "namesMareg",   NC_STRING,1, &dim0[0], &ids[3]));
	err_trap(nc_def_var(ncid, "Greens",       NC_FLOAT, 2, dim2,     &ids[4]));

	/* Set a deflation level of 5 (4, zero based) and shuffle for variables */
	err_trap(nc_def_var_deflate(ncid, ids[4], 1, 1, 4));
	err_trap(nc_put_vara_float(ncid,  ids[4], start, count, work));

	/* ---- Variables Attributes --------- */
	err_trap(nc_put_att_text(ncid, ids[0], "Description", 17, "Time at maregraph"));
	err_trap(nc_put_att_text(ncid, ids[0], "units", 7, "SECONDS"));
	err_trap(nc_put_att_text(ncid, ids[1], "Description", 23, "Longitude of maregraphs"));
	err_trap(nc_put_att_text(ncid, ids[2], "Description", 22, "Latitude of maregraphs"));
	err_trap(nc_put_att_text(ncid, ids[3], "Description", 29, "Code names for each maregraph"));
	err_trap(nc_put_att_text(ncid, ids[4], "Description", 69, "G array (transposed) of the Green functions: Nprism x Nmareg * Ntimes"));

	/* ---- Global Attributes ------------ */
	err_trap(nc_put_att_text(ncid, NC_GLOBAL, "Institution", 10, "Mirone Tec"));
#ifdef I_AM_MEX
	err_trap(nc_put_att_text(ncid, NC_GLOBAL, "Description", 24, "Created by Mirone-NSWING"));
#else
	err_trap(nc_put_att_text(ncid, NC_GLOBAL, "Description", 17, "Created by NSWING"));
#endif
	err_trap(nc_put_att_text(ncid, NC_GLOBAL, "History", strlen(hist), hist));
	err_trap(nc_put_att_int(ncid,  NC_GLOBAL, "Number of maregraphs", NC_INT, 1, &n_maregs));

	err_trap(nc_enddef (ncid));

	x = (double *)mxMalloc(sizeof(double) * n_maregs);
	y = (double *)mxMalloc(sizeof(double) * n_maregs);

	for (k = 0; k < n_maregs; k++) {
		ix = lcum_p[k] % nest->hdr[lev].nx;
		iy = lcum_p[k] / nest->hdr[lev].nx;
		x[k] = nest->hdr[lev].x_min + ix * nest->hdr[lev].x_inc;
		y[k] = nest->hdr[lev].y_min + iy * nest->hdr[lev].y_inc;
	}

	err_trap(nc_put_var_double(ncid, ids[0], t));
	err_trap(nc_put_var_double(ncid, ids[1], x));
	err_trap(nc_put_var_double(ncid, ids[2], y));
	err_trap(nc_put_var_string(ncid, ids[3], (const char **)names));
	mxFree(x); 
	mxFree(y); 

	return (ncid);
}

/* -------------------------------------------------------------------- */
int write_maregs_nc(struct nestContainer *nest, char *fname, float *work, double *t, unsigned int *lcum_p,
	char *names[], char hist[], int n_maregs, unsigned int n_times, int lev) {
	/* Save the maregraphs time series in a netCDF file. This version is for the NO Kabas case.
	   t        - is the vector of times
	   work     - array of maregraphs with one per column
	   n_maregs - number of maregraphs
	   n_times  - length(t)
	*/

	int     k, ix, iy, ncid = -1, status, dim0[3], dim2[2], ids[6];
	int    *maregs_vec;
	size_t	start[2] = {0,0}, count[2];
	double *x, *y;

	count[0] = n_times;	count[1] = n_maregs;

	if ((status = nc_create(fname, NC_NETCDF4, &ncid)) != NC_NOERR) {
		mexPrintf("NSWING: Unable to create file -- %s -- exiting\n", fname);
		return(-1);
	}

	/* ---- Define dimensions ------------ */
	err_trap(nc_def_dim(ncid, "time",   (size_t)n_times,  &dim0[0]));
	err_trap(nc_def_dim(ncid, "count",  (size_t)n_maregs, &dim0[1]));

	/* ---- Define variables ------------- */
	dim2[0] = dim0[0];                   dim2[1] = dim0[1];
	err_trap(nc_def_var(ncid, "time",    NC_DOUBLE,1, &dim0[0], &ids[0]));
	err_trap(nc_def_var(ncid, "count",   NC_INT,   1, &dim0[1], &ids[1]));
	if (nest->isGeog) {
		err_trap(nc_def_var(ncid, "lonMareg", NC_DOUBLE,1, &dim0[1], &ids[2]));
		err_trap(nc_def_var(ncid, "latMareg", NC_DOUBLE,1, &dim0[1], &ids[3]));
	}
	else {
		err_trap(nc_def_var(ncid, "xMareg",   NC_DOUBLE,1, &dim0[1], &ids[2]));
		err_trap(nc_def_var(ncid, "yMareg",   NC_DOUBLE,1, &dim0[1], &ids[3]));
	}
	err_trap(nc_def_var(ncid, "NamesMareg",   NC_STRING,1, &dim0[1], &ids[4]));
	err_trap(nc_def_var(ncid, "maregs",       NC_FLOAT, 2, dim2,     &ids[5]));

	/* Set a deflation level of 5 (4, zero based) and shuffle for variables */
	err_trap(nc_def_var_deflate(ncid, ids[5], 1, 1, 4));
	err_trap(nc_put_vara_float(ncid, ids[5], start, count, work));

	/* ---- Global Attributes ------------ */
	err_trap(nc_put_att_text(ncid, NC_GLOBAL, "Institution", 10, "Mirone Tec"));
#ifdef I_AM_MEX
	err_trap(nc_put_att_text(ncid, NC_GLOBAL, "Description", 24, "Created by Mirone-NSWING"));
#else
	err_trap(nc_put_att_text(ncid, NC_GLOBAL, "Description", 17, "Created by NSWING"));
#endif
	err_trap(nc_put_att_text(ncid, NC_GLOBAL, "History", strlen(hist), hist));
	err_trap(nc_put_att_int(ncid,  NC_GLOBAL, "Number of maregraphs", NC_INT, 1, &n_maregs));

	err_trap(nc_enddef (ncid));

	x = (double *)mxMalloc(sizeof(double) * n_maregs);
	y = (double *)mxMalloc(sizeof(double) * n_maregs);
	maregs_vec = (int *)mxMalloc(sizeof(int) * n_maregs);

	for (k = 0; k < n_maregs; k++) {
		ix = lcum_p[k] % nest->hdr[lev].nx;
		iy = lcum_p[k] / nest->hdr[lev].nx;
		x[k] = nest->hdr[lev].x_min + ix * nest->hdr[lev].x_inc;
		y[k] = nest->hdr[lev].y_min + iy * nest->hdr[lev].y_inc;
		maregs_vec[k] = k + 1;
	}

	err_trap(nc_put_var_double(ncid, ids[0], t));
	err_trap(nc_put_var_int   (ncid, ids[1], maregs_vec));
	err_trap(nc_put_var_double(ncid, ids[2], x));
	err_trap(nc_put_var_double(ncid, ids[3], y));
	err_trap(nc_put_var_string(ncid, ids[4], (const char **)names));
	mxFree(x); 
	mxFree(y); 
	mxFree(maregs_vec); 

	err_trap(nc_close(ncid)); 

	return (0);
}

/* -------------------------------------------------------------------- */
int open_anuga_sww (struct nestContainer *nest, char *fname_sww, char hist[], int *ids, unsigned int i_start,
	unsigned int j_start, unsigned int i_end, unsigned int j_end, double xMinOut, double yMinOut, int lev) {

	/* Open and initialize a ANUGA netCDF file for writing ---------------- */
	int ncid = -1, status, dim0[5], dim2[2], dim3[2], *volumes, *vertices;
	unsigned int m, n, nx, ny, nVolumes, nPoints;
	unsigned int i, j, k, m_nx, m1_nx, v1, v2, v3, v4;
	float dummy2[2], *x, *y, yr, *tmp;
	float z_min = nest->hdr[lev].z_min;
	float z_max = nest->hdr[lev].z_max;
	double dummy, nan, faultPolyX[11], faultPolyY[11], faultSlip[10], faultStrike[10], 
	       faultDip[10], faultRake[10], faultWidth[10], faultDepth[10];
	double dtx = nest->hdr[lev].x_inc;
	double dty = nest->hdr[lev].y_inc;

	if ( (status = nc_create (fname_sww, NC_NETCDF4, &ncid)) != NC_NOERR) {
		mexPrintf ("NSWING: Unable to create file -- %s -- exiting\n", fname_sww);
		return(-1);
	}

	/* ---- Define dimensions ------------ */
	nx = i_end - i_start;		ny = j_end - j_start;
	nVolumes = (nx - 1) * (ny - 1) * 2;
	nPoints = nx * ny;
	err_trap (nc_def_dim (ncid, "number_of_volumes",  (size_t) nVolumes, &dim0[0]));
	err_trap (nc_def_dim (ncid, "number_of_vertices", (size_t) 3, &dim0[1]));
	err_trap (nc_def_dim (ncid, "numbers_in_range",   (size_t) 2, &dim0[2]));
	err_trap (nc_def_dim (ncid, "number_of_points",   (size_t) nPoints, &dim0[3]));
	err_trap (nc_def_dim (ncid, "number_of_timesteps", NC_UNLIMITED, &dim0[4]));

	/* ---- Define variables ------------- */
	dim2[0] = dim0[4];                              dim2[1] = dim0[3];
	dim3[0] = dim0[0];                              dim3[1] = dim0[1];
	err_trap (nc_def_var (ncid, "x",                NC_FLOAT,1, &dim0[3], &ids[0]));
	err_trap (nc_def_var (ncid, "y",                NC_FLOAT,1, &dim0[3], &ids[1]));
	err_trap (nc_def_var (ncid, "z",                NC_FLOAT,1, &dim0[3], &ids[2]));
	err_trap (nc_def_var (ncid, "elevation",        NC_FLOAT,1, &dim0[3], &ids[3]));
	err_trap (nc_def_var (ncid, "elevation_range",  NC_FLOAT,1, &dim0[2], &ids[4]));
	err_trap (nc_def_var (ncid, "volumes",          NC_INT,  2, dim3,     &ids[5]));
	err_trap (nc_def_var (ncid, "time",             NC_DOUBLE,1,&dim0[4], &ids[6]));
	err_trap (nc_def_var (ncid, "stage",            NC_FLOAT,2, dim2,     &ids[7]));
	err_trap (nc_def_var (ncid, "stage_range",      NC_FLOAT,1, &dim0[2], &ids[8]));
	err_trap (nc_def_var (ncid, "xmomentum",        NC_FLOAT,2, dim2,     &ids[9]));
	err_trap (nc_def_var (ncid, "xmomentum_range",  NC_FLOAT,1, &dim0[2], &ids[10]));
	err_trap (nc_def_var (ncid, "ymomentum",        NC_FLOAT,2, dim2,     &ids[11]));
	err_trap (nc_def_var (ncid, "ymomentum_range",  NC_FLOAT,1, &dim0[2], &ids[12]));

	/* Set a deflation level of 5 (4 zero based) and shuffle for variables */
	err_trap(nc_def_var_deflate(ncid, ids[0], 1, 1, 4));
	err_trap(nc_def_var_deflate(ncid, ids[1], 1, 1, 4));
	err_trap(nc_def_var_deflate(ncid, ids[2], 1, 1, 4));
	err_trap(nc_def_var_deflate(ncid, ids[3], 1, 1, 4));
	err_trap(nc_def_var_deflate(ncid, ids[5], 1, 1, 4));
	err_trap(nc_def_var_deflate(ncid, ids[7], 1, 1, 4));
	err_trap(nc_def_var_deflate(ncid, ids[9], 1, 1, 4));
	err_trap(nc_def_var_deflate(ncid, ids[11],1, 1, 4));

	/* ---- Global Attributes ------------ */
	err_trap(nc_put_att_text(ncid,   NC_GLOBAL, "institution", 10, "Mirone Tec"));
	err_trap(nc_put_att_text(ncid,   NC_GLOBAL, "description", 22, "Created by Mirone-NSWING"));
	err_trap(nc_put_att_text(ncid,   NC_GLOBAL, "History", strlen(hist), hist));
	err_trap(nc_put_att_double(ncid, NC_GLOBAL, "xllcorner", NC_DOUBLE, 1, &xMinOut));
	err_trap(nc_put_att_double(ncid, NC_GLOBAL, "yllcorner", NC_DOUBLE, 1, &yMinOut));
	dummy = 29;	err_trap(nc_put_att_double(ncid, NC_GLOBAL, "zone", NC_DOUBLE, 1, &dummy));
	dummy = 0;	err_trap(nc_put_att_double(ncid, NC_GLOBAL, "starttime", NC_DOUBLE, 1, &dummy));
	dummy = 500000;	err_trap(nc_put_att_double(ncid, NC_GLOBAL, "false_easting", NC_DOUBLE, 1, &dummy));
	dummy = 0;	err_trap(nc_put_att_double(ncid, NC_GLOBAL, "false_northing", NC_DOUBLE, 1, &dummy));
	err_trap(nc_put_att_text(ncid,   NC_GLOBAL, "datum", 5, "wgs84"));
	err_trap(nc_put_att_text(ncid,   NC_GLOBAL, "projection", 3, "UTM"));
	err_trap(nc_put_att_text(ncid,   NC_GLOBAL, "units", 1, "m"));
	/* Initialize the following attribs with NaNs. A posterior call will eventualy fill them with the right values */
	nan = mxGetNaN();
	if (nan == 0) nan = (float)loc_nan.d;	/* Dirty hack. With the Intel compiler for example mxGetNaN() returns 0*/
	for (i = 0; i < 10; i++) {
		faultPolyX[i] = faultPolyY[i] = faultSlip[i] = faultDip[i] = faultStrike[i] = 
				faultRake[i] = faultWidth[i] = faultDepth[i] = nan;
	}
	faultPolyX[10] = faultPolyY[10] = nan;		/* Those have an extra element */
	err_trap(nc_put_att_double(ncid, NC_GLOBAL, "faultPolyX",  NC_DOUBLE, 11, faultPolyX));
	err_trap(nc_put_att_double(ncid, NC_GLOBAL, "faultPolyY",  NC_DOUBLE, 11, faultPolyY));
	err_trap(nc_put_att_double(ncid, NC_GLOBAL, "faultStrike", NC_DOUBLE, 10, faultStrike));
	err_trap(nc_put_att_double(ncid, NC_GLOBAL, "faultSlip",   NC_DOUBLE, 10, faultSlip));
	err_trap(nc_put_att_double(ncid, NC_GLOBAL, "faultDip",    NC_DOUBLE, 10, faultDip));
	err_trap(nc_put_att_double(ncid, NC_GLOBAL, "faultRake",   NC_DOUBLE, 10, faultRake));
	err_trap(nc_put_att_double(ncid, NC_GLOBAL, "faultWidth",  NC_DOUBLE, 10, faultWidth));
	err_trap(nc_put_att_double(ncid, NC_GLOBAL, "faultDepth",  NC_DOUBLE, 10, faultDepth));

	/* ---- Write the vector coords ------ */
	x = (float *)mxMalloc(sizeof(float) * (nx * ny));
	y = (float *)mxMalloc(sizeof(float) * (nx * ny));
	vertices = (int *)mxMalloc(sizeof(unsigned int) * (nx * ny));
	volumes  = (int *)mxMalloc(sizeof(unsigned int) * (nVolumes * 3));

	/* Construct 2 triangles per 'rectangular' element */
	i = 0;
	for (m = 0; m < ny; m++) {		/* By rows    - Y */
		yr = (float)(m * dty);
		for (n = 0; n < nx; n++) {	/* By columns - X */
			x[i] = (float)(n * dtx);
			y[i] = yr;
			vertices[i] = i;
			i++;
		}
	}

	for (n = i = 0; n < nx - 1; n++) {			/* X */
		for (m = 0; m < ny - 1; m++) {			/* Y */
			m_nx = m * nx;		m1_nx = (m + 1) * nx;
			v1 = vertices[n + m_nx];	v2 = vertices[n + 1 + m_nx];
			v3 = vertices[n + 1 + m1_nx];	v4 = vertices[n + m1_nx];
			volumes[i] = v1;	volumes[i+1] = v2;	volumes[i+2] = v3;
			i += 3;
			volumes[i] = v1;	volumes[i+1] = v3;	volumes[i+2] = v4;
			i += 3;
		}
	}

	mxFree ((void *)vertices);

	err_trap(nc_enddef(ncid));
	err_trap(nc_put_var_float(ncid, ids[0], x));
	err_trap(nc_put_var_float(ncid, ids[1], y));
	mxFree((void *)x); 
	mxFree((void *)y); 

	tmp = (float *)mxMalloc(sizeof (float) * (nx * ny));
	for (j = j_start, k = 0; j < j_end; j++) {
		for (i = i_start; i < i_end; i++)
			tmp[k++] = (float)-nest->bat[lev][ijs(i,j,nest->hdr[lev].nx)];
	}

	err_trap(nc_put_var_float(ncid, ids[2], tmp));	/* z */

	err_trap(nc_put_var_float(ncid, ids[3], tmp));	/* elevation */
	dummy2[0] = z_min;		dummy2[1] = z_max;
	err_trap(nc_put_var_float(ncid, ids[4], dummy2));	/* elevation_range */
	err_trap(nc_put_var_int(ncid, ids[5], volumes));

	mxFree((void *)tmp);
	mxFree((void *)volumes);

	return (ncid);
}

/* --------------------------------------------------------------------------- */
void write_anuga_slice(struct nestContainer *nest, int ncid, int z_id, unsigned int i_start, unsigned int j_start,
	unsigned int i_end, unsigned int j_end, float *work, size_t *start, size_t *count, float *slice_range,
	int idx, int with_land, int lev) {
	/* Write a slice of either STAGE, XMOMENTUM or YMOMENTUM of a Anuga's .sww netCDF file */
	unsigned int row, col, ij, k, ncl;

	ncl = (i_end - i_start) * (j_end - j_start);
	k = 0;

	if (idx == 1) {
		if (!with_land) {		/* Land nodes are kept = 0 */
			if (i_end == nest->hdr[lev].nx && j_end == nest->hdr[lev].ny) {        /* Full Region */
				for (ij = 0; ij < nest->hdr[lev].nm; ij++)
					work[ij] = (float)nest->etad[lev][ij];              /* Anuga calls this -> stage */
			}
			else {		/* A sub-region */
				for (row = j_start; row < j_end; row++)
					for (col = i_start; col < i_end; col++)
						work[k++] = (float)nest->etad[lev][ij_grd(col, row, nest->hdr[lev])];
			}
		}
		else {
			if (i_end == nest->hdr[lev].nx && j_end == nest->hdr[lev].ny) {        /* Full Region */
				for (ij = 0; ij < nest->hdr[lev].nm; ij++)
					work[ij] = (nest->htotal_d[lev][ij] < EPS3) ? (float)-nest->bat[lev][ij] : (float)nest->etad[lev][ij];
			}
			else {		/* A sub-region */
				for (row = j_start; row < j_end; row++) {
					for (col = i_start; col < i_end; col++) {
						ij = ij_grd(col, row, nest->hdr[lev]);
						work[k++] = (nest->htotal_d[lev][ij] < EPS3) ? (float)-nest->bat[lev][ij] :
						           (float)nest->etad[lev][ij]; 
					}
				}
			}
		}
	}
	else if (idx == 2) {	/* X momentum */
		if (i_end == nest->hdr[lev].nx && j_end == nest->hdr[lev].ny) {
			for (ij = 0; ij < nest->hdr[lev].nm; ij++)
				work[ij] = (float)nest->fluxm_d[lev][ij];
		}
		else {		/* A sub-region */
			for (row = j_start; row < j_end; row++)
				for (col = i_start; col < i_end; col++)
					work[k++] = (float)nest->fluxm_d[lev][ij_grd(col, row, nest->hdr[lev])];
		}
	}
	else {			/* Y momentum */
		if (i_end == nest->hdr[lev].nx && j_end == nest->hdr[lev].ny) {
			for (ij = 0; ij < nest->hdr[lev].nm; ij++)
				work[ij] = (float)nest->fluxn_d[lev][ij];
		}
		else {		/* A sub-region */
			for (row = j_start; row < j_end; row++)
				for (col = i_start; col < i_end; col++)
					work[k++] = (float)nest->fluxn_d[lev][ij_grd(col, row, nest->hdr[lev])];
		}
	}

	/* ----------- Find the min/max of this slice --------- */
	for (k = 0; k < ncl; k++) {
		slice_range[1] = MAX(work[k], slice_range[1]);
		slice_range[0] = MIN(work[k], slice_range[0]);
	}

	err_trap (nc_put_vara_float (ncid, z_id, start, count, work));
}

/* --------------------------------------------------------------------------- */
void err_trap_(int status) {
	if (status != NC_NOERR)	
		mexPrintf ("NSWING: error at line: %d\t and errorcode = %d\n", __LINE__, status);
}
#endif		/* end HAVE_NETCDF */

/* --------------------------------------------------------------------
 * solves non linear continuity equation w/ cartesian coordinates
 * Computes water depth (htotal) needed for friction and
 * moving boundary algorithm
 * htotal < 0 - dry cell htotal (m) above still water
 * htotal > 0 - wet cell with htotal (m) of water depth
 *
 *		Updates only etad and htotal_d
 * -------------------------------------------------------------------- */
void mass(struct nestContainer *nest, int lev) {

	int row, col;
	int cm1, rm1;			/* previous column (cm1 = col -1) and row (rm1 = row - 1) */
	unsigned int ij;
	double dtdx, dtdy, dd = 0, zzz;
	double *etaa, *etad, *htotal_d, *bat, *fluxm_a, *fluxn_a;

	etaa     = nest->etaa[lev];          etad    = nest->etad[lev];
	htotal_d = nest->htotal_d[lev];      bat     = nest->bat[lev];
	fluxm_a  = nest->fluxm_a[lev];       fluxn_a = nest->fluxn_a[lev];

	dtdx = nest->dt[lev] / nest->hdr[lev].x_inc;
	dtdy = nest->dt[lev] / nest->hdr[lev].y_inc;

	for (row = 0; row < nest->hdr[lev].ny; row++) {
		ij = row * nest->hdr[lev].nx;
		rm1 = (row == 0) ? 0 : nest->hdr[lev].nx;
		for (col = 0; col < nest->hdr[lev].nx; col++) {
			/* case ocean and non permanent dry area */
			if (bat[ij] > MAXRUNUP) {
				cm1 = (col == 0) ? 0 : 1;
				zzz = etaa[ij] - dtdx * (fluxm_a[ij] - fluxm_a[ij-cm1]) - dtdy * (fluxn_a[ij] - fluxn_a[ij-rm1]);
				//if (fabs(zzz) < EPS6) zzz = 0;
				dd = zzz + bat[ij];

				/* wetable zone */
				if (dd > EPS10) {
					htotal_d[ij] = dd;
					etad[ij] = zzz;
				}
				else {			/* over dry areas htotal is null and eta follows bat */
					htotal_d[ij] = 0;
					etad[ij] = -bat[ij];
				}

				if (nest->do_long_beach  && (lev == nest->writeLevel) && bat[ij] > 0 && dd < EPS1)
					nest->long_beach[lev][ij] = 1;
				if (nest->do_short_beach && (lev == nest->writeLevel) && bat[ij] < 0 && zzz > EPS1)
					nest->short_beach[lev][ij] = 1;
			}
			else {			/* over dry areas htotal is null and eta follows bat */
				etad[ij] = -bat[ij];
			}
			ij++;
		}
	}
}

/* ---------------------------------------------------------------------- */
/* Send waves through a boundary */
/* ---------------------------------------------------------------------- */
void wave_maker(struct nestContainer *nest) {
	int col, row;
	int64_t ij;

	if ((nest->bnc_border[0] != 0) || (nest->bnc_border[2] != 0)) {         /* West or East borders */
		col = (nest->bnc_border[0] != 0) ? 0 : nest->hdr[0].nx - 1;
		for (row = 0; row < nest->hdr[0].ny; row++) {
			ij = ij_grd(col,row,nest->hdr[0]);
			if (nest->bat [0][ij] < EPS5) {
				nest->etad[0][ij] = -nest->bat[0][ij];
				continue;
			}
			nest->etad[0][ij] = nest->bnc_var_z_interp[row];
		}
	}
	else {    /* South or North borders */
		row = (nest->bnc_border[1] != 0) ? 0 : nest->hdr[0].ny - 1;
		for (col = 0; col < nest->hdr[0].nx; col++) {
			if (nest->bat [0][ij_grd(col,row,nest->hdr[0])] < EPS5) {
				nest->etad[0][ij_grd(col,row,nest->hdr[0])] = -nest->bat[0][ij_grd(col,row,nest->hdr[0])];
				continue;
			}
			nest->etad[0][ij_grd(col,row,nest->hdr[0])] = nest->bnc_var_z_interp[col];
		}
	}
}

/* --------------------------------------------------------------------------- */
void wall_it(struct nestContainer *nest) {
	/* Set up vertical walls in all other boundaries but from the one where water goes in */

	if (nest->bnc_border[0] != 0) {		/* West border */
		wall_two(nest, 0, nest->hdr[0].nx, nest->hdr[0].ny - 2, nest->hdr[0].ny);   /* Wall the North border */
		wall_two(nest, 0, nest->hdr[0].nx, 0, 2);                                   /* Wall the South border */
		wall_two(nest, nest->hdr[0].nx - 2, nest->hdr[0].nx, 0, nest->hdr[0].ny);   /* Wall the East border */
	}
	else if (nest->bnc_border[1] != 0) {		/* South border */
		wall_two(nest, 0, 2, 0, nest->hdr[0].ny);                                   /* Wall the West border */
		wall_two(nest, nest->hdr[0].nx - 2, nest->hdr[0].nx, 0, nest->hdr[0].ny);   /* Wall the East border */
		wall_two(nest, 0, nest->hdr[0].nx, nest->hdr[0].ny - 2, nest->hdr[0].ny);   /* Wall the North border */
	}
	else if (nest->bnc_border[2] != 0) {		/* East border */
		wall_two(nest, 0, nest->hdr[0].nx, nest->hdr[0].ny - 2, nest->hdr[0].ny);   /* Wall the North border */
		wall_two(nest, 0, nest->hdr[0].nx, 0, 2);                                   /* Wall the South border */
		wall_two(nest, 0, 2, 0, nest->hdr[0].ny);                                   /* Wall the West border */
	}
	else if (nest->bnc_border[3] != 0) {		/* Noth border */
		wall_two(nest, 0, 2, 0, nest->hdr[0].ny);                                   /* Wall the West border */
		wall_two(nest, 0, nest->hdr[0].nx, 0, 2);                                   /* Wall the South border */
		wall_two(nest, nest->hdr[0].nx - 2, nest->hdr[0].nx, 0, nest->hdr[0].ny);   /* Wall the East border */
	}
}

/* --------------------------------------------------------------------------- */
void wall_two(struct nestContainer *nest, int ot1, int ot2, int in1, int in2) {
	/* Set up a vertical wall boundary with a thickness of (ot2 - ot1 + 1)
	   The wall's height is set to minimum needed so that when we visualize it
	   in a 3D generic nc file, colors are not smashed by an unneeded tal wall
	*/
	int i, j;
	double wall_height = -MAXRUNUP;
	wall_height = -MIN(nest->hdr[0].z_max, wall_height);	/* F. crazy but MSVC doesn't allow to do both in one */

	for (i = ot1; i < ot2; i++) {       /* Loop over wall X length */
		for (j = in1; j < in2; j++) {   /* Loop over wall Y length */
			if (nest->bat[0][ij_grd(i,j,nest->hdr[0])] < MAXRUNUP) continue;	/* Already high enough */
			nest->bat[0][ij_grd(i,j,nest->hdr[0])] = wall_height;
		}
	}
}

/* ---------------------------------------------------------------------- */
/* open boundary condition */
/* ---------------------------------------------------------------------- */
void openb(struct grd_header hdr, double *bat, double *fluxm_d, double *fluxn_d, double *etad, struct nestContainer *nest) {

	int i, j;
	double uh, zz, d__1, d__2;

	/* ----- first column (South border) */
	j = 0;
	for (i = 1; i < hdr.nx - 1; i++) {
		if (bat[ij_grd(i,j,hdr)] < EPS5) {
			etad[ij_grd(i,j,hdr)] = -bat[ij_grd(i,j,hdr)];
			continue;
		}
		uh = (fluxm_d[ij_grd(i,j,hdr)] + fluxm_d[ij_grd(i-1,j,hdr)]) * 0.5;
		d__2 = fluxn_d[ij_grd(i,j,hdr)];
		zz = sqrt(uh * uh + d__2 * d__2) / sqrt(NORMAL_GRAV * bat[ij_grd(i,j,hdr)]);
		if (d__2 > 0) zz *= -1;
		etad[ij_grd(i,j,hdr)] = zz;
	}

	/* ------ last column (North border) */
	j = hdr.ny - 1;
	for (i = 1; i < hdr.nx - 1; i++) {
		if (bat[ij_grd(i,j,hdr)] > EPS5) {
			uh = (fluxm_d[ij_grd(i,j,hdr)] + fluxm_d[ij_grd(i-1,j,hdr)]) * 0.5;
			d__2 = fluxn_d[ij_grd(i,j-1,hdr)];
			zz = sqrt(uh * uh + d__2 * d__2) / sqrt(NORMAL_GRAV * bat[ij_grd(i,j,hdr)]);
			if (fluxn_d[ij_grd(i,j-1,hdr)] < 0) zz *= -1;
			if (fabs(zz) <= EPS5) zz = 0;
			etad[ij_grd(i,j,hdr)] = zz;
		} 
		else
			etad[ij_grd(i,j,hdr)] = -bat[ij_grd(i,j,hdr)];
	}

	/* ------ first row (West border) */
	i = 0;
	for (j = 1; j < hdr.ny - 1; j++) {
		if (bat[ij_grd(i,j,hdr)] < EPS5) {
			etad[ij_grd(i,j,hdr)] = -bat[ij_grd(i,j,hdr)];
			continue;
		}
		if (bat[ij_grd(i,j-1,hdr)] > EPS5)
			uh = (fluxn_d[ij_grd(i,j,hdr)] + fluxn_d[ij_grd(i,j-1,hdr)]) * 0.5;
		else
			uh = fluxn_d[ij_grd(i,j,hdr)];
	
		d__2 = fluxm_d[ij_grd(i,j,hdr)];
		zz = sqrt(uh * uh + d__2 * d__2) / sqrt(NORMAL_GRAV * bat[ij_grd(i,j,hdr)]);
		if (fluxm_d[ij_grd(i,j,hdr)] > 0) zz *= -1;
		if (fabs(zz) <= EPS5) zz = 0;
		etad[ij_grd(i,j,hdr)] = zz;
	}

	/* ------- last row (East border) */
	i = hdr.nx - 1;
	for (j = 1; j < hdr.ny - 1; j++) {
		if (bat[ij_grd(i,j,hdr)] > EPS5) {
			uh = (fluxn_d[ij_grd(i,j,hdr)] + fluxn_d[ij_grd(i,j-1,hdr)]) * 0.5;
			d__2 = fluxm_d[ij_grd(i-1,j,hdr)];
			zz = sqrt(uh * uh + d__2 * d__2) / sqrt(NORMAL_GRAV * bat[ij_grd(i,j,hdr)]);
			if (fluxm_d[ij_grd(i-1,j,hdr)] < 0) zz *= -1;
			etad[ij_grd(i,j,hdr)] = zz;
		} 
		else
			etad[ij_grd(i,j,hdr)] = -bat[ij_grd(i,j,hdr)];
	}

	/* -------- first row & first column (SW corner) */
	if (nest->bnc_border[1] == 0) { 
		if (bat[0] > EPS5) {
			zz = sqrt(fluxm_d[0] * fluxm_d[0] + fluxn_d[0] * fluxn_d[0]) / sqrt(NORMAL_GRAV * bat[0]);
			if (fluxm_d[0] > 0 || fluxn_d[0] > 0) zz *= -1;
			if (fabs(zz) <= EPS5) zz = 0;
			etad[0] = zz;
		} 
		else
			etad[0] = -bat[0];
	}

	/* -------- last row & first column */
	if (bat[ij_grd(hdr.nx-1,0,hdr)] > EPS5) {
		d__1 = fluxm_d[ij_grd(hdr.nx-2,0,hdr)];
		d__2 = fluxn_d[ij_grd(hdr.nx-1,0,hdr)];
		zz = sqrt(d__1 * d__1 + d__2 * d__2) / sqrt(NORMAL_GRAV * bat[ij_grd(hdr.nx-1,0,hdr)]);
		if (fluxm_d[ij_grd(hdr.nx-2,0,hdr)] < 0 || fluxn_d[ij_grd(hdr.nx-1,0,hdr)] > 0) zz *= -1;
		if (fabs(zz) <= EPS5) zz = 0;
		etad[ij_grd(0,hdr.ny-1,hdr)] = zz;
	} 
	else
		etad[ij_grd(0,hdr.ny-1,hdr)] = -bat[ij_grd(0,hdr.ny-1,hdr)];

	/* -------- first row & last column */
	if (bat[ij_grd(0,hdr.ny-1,hdr)] > EPS5) {
		d__1 = fluxm_d[ij_grd(0,hdr.ny-1,hdr)];
		d__2 = fluxn_d[ij_grd(0,hdr.ny-2,hdr)];
		zz = sqrt(d__1 * d__1 + d__2 * d__2) / sqrt(NORMAL_GRAV * bat[ij_grd(0,hdr.ny-1,hdr)]);
		if (fluxm_d[ij_grd(0,hdr.ny-1,hdr)] > 0 || fluxn_d[ij_grd(0,hdr.ny-2,hdr)] < 0) zz = -zz;
		if (fabs(zz) <= EPS5) zz = 0;
		etad[ij_grd(0,hdr.ny-1,hdr)] = zz;
	} 
	else
		etad[ij_grd(0,hdr.ny-1,hdr)] = -bat[ij_grd(0,hdr.ny-1,hdr)];

	/* ---------- last row & last column */
	if (bat[ij_grd(hdr.nx-1,hdr.ny-1,hdr)] > EPS5) {
		d__1 = fluxm_d[ij_grd(hdr.nx-2,hdr.ny-1,hdr)];
		d__2 = fluxn_d[ij_grd(hdr.nx-1,hdr.ny-2,hdr)];
		zz = sqrt(d__1 * d__1 + d__2 * d__2) / sqrt(NORMAL_GRAV * bat[ij_grd(hdr.nx-1,hdr.ny-1,hdr)]);
		if (fluxm_d[ij_grd(hdr.nx-2,hdr.ny-1,hdr)] < 0 || fluxn_d[ij_grd(hdr.nx-1,hdr.ny-2,hdr)] < 0) zz *= -1;
		etad[ij_grd(hdr.nx-1,hdr.ny-1,hdr)] = zz;
	} 
	else
		etad[ij_grd(hdr.nx-1,hdr.ny-1,hdr)] = -bat[ij_grd(hdr.nx-1,hdr.ny-1,hdr)];
}

/* --------------------------------------------------------------------- */
/* update eta and fluxes */
/* --------------------------------------------------------------------- */
void update(struct nestContainer *nest, int lev) {
	memcpy(nest->etaa[lev],    nest->etad[lev],    nest->hdr[lev].nm * sizeof(double));
	memcpy(nest->fluxm_a[lev], nest->fluxm_d[lev], nest->hdr[lev].nm * sizeof(double));
	memcpy(nest->fluxn_a[lev], nest->fluxn_d[lev], nest->hdr[lev].nm * sizeof(double));
	memcpy(nest->htotal_a[lev],nest->htotal_d[lev],nest->hdr[lev].nm * sizeof(double));
}


/* -------------------------------------------------------------------------
 * Solve nonlinear momentum equation, cartesian coordinates with moving boundary
 *
 *    dx,dy,dt: grid spacing and time step
 *    ifriction: 0/1 to use manning coefficient manning_coef
 *    icor: 0/1 to consider Coriolis
 *    htotal_a / htotal_d: total water thickness previous/next time step
 *    bat: bathymetry
 *    etad: water heigth, in dry areas equals topography (positive)
 *    fluxm_a,fluxn_a: fluxes M and N previous time step

 *    fluxm_d,fluxn_d: fluxes M and N next time step (output)
 *    vex,vey: particle velocities next time step (output)
 *
 *		Updates fluxm_d and fluxn_d
 * ---------------------------------------------------------------------- */
void moment_M(struct nestContainer *nest, int lev) {

	unsigned int ij;
	int first, last, jupe, row, col;
	int valid_vel;
	int cm1, rm1;			/* previous column (cm1 = col - 1) and row (rm1 = row - 1) */
	int cp1, rp1;			/* next column (cp1 = col + 1) and row (rp1 = row + 1) */
	int rm2, cp2;
	double xp, xqe, xqq, ff = 0, dd, df, cte, f_limit;
	double advx, dtdx, dtdy, advy, rlat;
	double dpa_ij, dpa_ij_rp1, dpa_ij_rm1, dpa_ij_cm1, dpa_ij_cp1;

	double dt, manning, *bat, *htotal_a, *htotal_d, *etad, *fluxm_a, *fluxm_d, *fluxn_a, *fluxn_d, *vex, *r4m;
	struct grd_header hdr;

	hdr      = nest->hdr[lev];             vex      = nest->vex[lev];
	dt       = nest->dt[lev];              manning  = nest->manning[lev];
	bat      = nest->bat[lev];             etad     = nest->etad[lev];
	htotal_a = nest->htotal_a[lev];        htotal_d = nest->htotal_d[lev];
	fluxm_a  = nest->fluxm_a[lev];         fluxm_d  = nest->fluxm_d[lev];
	fluxn_a  = nest->fluxn_a[lev];         fluxn_d  = nest->fluxn_d[lev];
	r4m      = nest->r4m[lev];

	dtdx = dt / hdr.x_inc;
	dtdy = dt / hdr.y_inc;

	/* fixes the width of the lateral buffer for linear aproximation */
	/* if jupe>nnx/2 and jupe>nny/2 linear model will be applied */
	if (lev > 0) {			/* We are in a call to a nested grid */
		jupe = 0;    first = 1;    last = 0;
	}
	else {
		jupe = 5;    first = 0;    last = 1;
	}

	if (nest->do_linear) jupe = 1e6;		/* A tricky way of imposing linearity */

	/* fixes friction parameter */
	cte = (manning != 0) ? manning * manning * dt * 4.9 : 0;

	memset(fluxm_d, 0, hdr.nm * sizeof(double));	/* Do this rather than seting to zero under looping conditions */

	/* main computation cycle fluxm_d */
	for (row = 0; row < hdr.ny - last; row++) {
		rp1 = (row < hdr.ny - 1) ? hdr.nx : 0;
		rm1 = (row == 0) ? 0 : hdr.nx;
		ij = row * hdr.nx - 1 + first;

		for (col = first; col < hdr.nx - 1; col++) {
			cp1 = 1;
			cp2 = (col < hdr.nx - 2) ? 2 : 1;
			cm1 = (col == 0) ? 0 : 1;
			ij++;
			/* no flux to permanent dry areas */
			if (bat[ij] <= MAXRUNUP) continue;

			/* Looks weird but it's faster than an IF case (branch prediction?) */
			dpa_ij = (dpa_ij = (htotal_d[ij] + htotal_a[ij] + htotal_d[ij+cp1] + htotal_a[ij+cp1]) * 0.25) > EPS5 ? dpa_ij : 0;
			xp = 0;

			valid_vel = TRUE;
			if (htotal_d[ij] > EPS5 && htotal_d[ij+cp1] > EPS5) {		/* case wet-wet */
				if (-bat[ij+cp1] >= etad[ij]) {				/* case b2 */
					df = dd = htotal_d[ij+cp1];
					valid_vel = FALSE;		/* It means we won't believe in the velocity estimated for this cell */
				}
				else if (-bat[ij] >= etad[ij+cp1]) {		/* case d2 */
					df = dd = htotal_d[ij];
					valid_vel = FALSE;
				}
				else {										/* case b3/d3 */
					dd = (htotal_d[ij] + htotal_d[ij+cp1]) * 0.5;
					if (dd < EPS5) dd = 0;
					df = dpa_ij;
				}
			}
			else if (htotal_d[ij] > EPS5 && htotal_d[ij+cp1] < EPS5 && etad[ij] >= etad[ij+cp1]) {	/* case a3/d1 wet-dry */
				if (bat[ij] > bat[ij+cp1])
					df = dd = etad[ij] - etad[ij+cp1];
				else
					df = dd = htotal_d[ij];
			}
			else if (htotal_d[ij] < EPS5 && htotal_d[ij+cp1] > EPS5 && etad[ij] <= etad[ij+cp1]) {	/* case b1 and c3 dry-wet */
				if (bat[ij] > bat[ij+cp1])
					df = dd = htotal_d[ij+cp1];
				else
					df = dd = etad[ij+cp1] - etad[ij];
			}
			else            /* other cases no moving boundary a1,a2,c1,c2 */
				goto L121;  /* Do this instead of a 'continue' to avoid another IF branch to account for velocity */ 

			/* disregards fluxes when dd is very small - pode ser EPS6 */
			if (dd < EPS4) goto L121;

			if (df < EPS4) df = EPS4;
			xqq = (fluxn_a[ij] + fluxn_a[ij+cp1] + fluxn_a[ij-rm1] + fluxn_a[ij+cp1-rm1]) * 0.25;
			ff = (manning != 0 && bat[ij] < nest->manning_depth) ? cte * sqrt(fluxm_a[ij] * fluxm_a[ij] + xqq * xqq) / pow(df, 2.333333) : 0;

			/* computes linear terms in cartesian coordinates */
			xp = (1 - ff) * fluxm_a[ij] - dtdx * NORMAL_GRAV * dd * (etad[ij+cp1] - etad[ij]);

			/* - if requested computes Coriolis term */
			if (nest->do_Coriolis) xp += r4m[row] * 2 * xqq;

			/* - total water depth is smaller than EPS3 >> linear */
			if (dpa_ij < EPS4) goto L120;
			/* - lateral buffer >> linear */
			if (col < jupe || col > (hdr.nx - jupe - 1) || row < jupe || row > (hdr.ny - jupe - 1))
				goto L120;

			/* - computes convection terms */
			advx = advy = 0;
			/* - upwind scheme for x-direction volume flux */
			if (fluxm_a[ij] < 0) {
				dpa_ij_cp1 = (htotal_d[ij+cp1] + htotal_a[ij+cp1] + htotal_d[ij+cp2] + htotal_a[ij+cp2]) * 0.25;
				if (dpa_ij_cp1 < EPS3 || htotal_d[ij+cp1] < EPS5)
					advx = -dtdx * (fluxm_a[ij] * fluxm_a[ij] / dpa_ij);
				else
					advx =  dtdx * (fluxm_a[ij+cp1]*fluxm_a[ij+cp1] / dpa_ij_cp1 - fluxm_a[ij]*fluxm_a[ij] / dpa_ij);
			}
			else {
				dpa_ij_cm1 = (htotal_d[ij-cm1] + htotal_a[ij-cm1] + htotal_d[ij] + htotal_a[ij]) * 0.25;
				if (dpa_ij_cm1 < EPS3 || htotal_d[ij] < EPS5)
					advx = dtdx * (fluxm_a[ij] * fluxm_a[ij] / dpa_ij);
				else
					advx = dtdx * (fluxm_a[ij] * fluxm_a[ij] / dpa_ij - fluxm_a[ij-cm1] * fluxm_a[ij-cm1] / dpa_ij_cm1);
			}

			/* - upwind scheme for y-direction volume flux */
			if (xqq < 0) {
				if (htotal_d[ij+rp1] < EPS5 || htotal_d[ij+cp1+rp1] < EPS5)
					advy = -dtdy * (fluxm_a[ij] * xqq / dpa_ij);

				else {
					dpa_ij_rp1 = (htotal_d[ij+rp1] + htotal_a[ij+rp1] + htotal_d[ij+cp1+rp1] + htotal_a[ij+cp1+rp1]) * 0.25;
					if (dpa_ij_rp1 < EPS5)
						advy = -dtdy * (fluxm_a[ij] * xqq / dpa_ij);

					else {
						xqe = (fluxn_a[ij+rp1] + fluxn_a[ij+cp1+rp1] + fluxn_a[ij] + fluxn_a[ij+cp1]) * 0.25;
						advy = dtdy * (fluxm_a[ij+rp1] * xqe / dpa_ij_rp1 - fluxm_a[ij] * xqq / dpa_ij);
					}
				}
			}
			else {
				if (htotal_d[ij-rm1] < EPS5 || htotal_d[ij+cp1-rm1] < EPS5)
					advy = dtdy * (fluxm_a[ij] * xqq / dpa_ij);

				else {
					dpa_ij_rm1 = (htotal_d[ij-rm1] + htotal_a[ij-rm1] + htotal_d[ij+cp1-rm1] + htotal_a[ij+cp1-rm1]) * 0.25;
					if (dpa_ij_rm1 < EPS5)
						advy = dtdy * (fluxm_a[ij] * xqq / dpa_ij);

					else {
						rm2 = (row < 2) ? 0 : 2 * hdr.nx;
						xqe = (fluxn_a[ij-rm1] + fluxn_a[ij+cp1-rm1] + fluxn_a[ij-rm2] + fluxn_a[ij+cp1-rm2]) * 0.25;
						advy = dtdy * (fluxm_a[ij] * xqq / dpa_ij - fluxm_a[ij-rm1] * xqe / dpa_ij_rm1);
					}
				}
			}

			/* adds linear+convection terms */
			xp = xp - advx - advy;
L120:
			xp /= (ff + 1);
#ifdef LIMIT_DISCHARGE
			if (fabs(xp) < EPS10)
				xp = 0;
			else {			/* Limit the discharge */
				f_limit = V_LIMIT * dd;
				if (xp > f_limit)
					xp = f_limit;
				else if (xp < -f_limit)
					xp = -f_limit;
			}
#endif
			fluxm_d[ij] = xp;

L121:
			if (nest->out_velocity_x && (lev == nest->writeLevel))
				vex[ij] = (valid_vel && dd > EPS3) ? xp / df : 0;
		}
	}
}

/* -------------------------------------------------------------------- */
void moment_N(struct nestContainer *nest, int lev) {

	unsigned int ij;
	int first, last, jupe, row, col;
	int valid_vel;
	int cm1, rm1;			/* previous column (cm1 = col - 1) and row (rm1 = row - 1) */
	int cp1, rp1;			/* next column (cp1 = col + 1) and row (rp1 = row + 1) */
	int cm2, rp2;
	double xq, xpe, xpp, ff = 0, dd, df, cte, f_limit;
	double advx, dtdx, dtdy, advy, rlat;
	double dqa_ij, dqa_ij_rp1, dqa_ij_rm1, dqa_ij_cm1, dqa_ij_cp1;

	double dt, manning, *bat, *htotal_a, *htotal_d, *etad, *fluxm_a, *fluxm_d, *fluxn_a, *fluxn_d, *vey, *r4n;
	struct grd_header hdr;

	hdr      = nest->hdr[lev];             vey      = nest->vey[lev];
	dt       = nest->dt[lev];              manning  = nest->manning[lev];
	bat      = nest->bat[lev];             etad     = nest->etad[lev];
	htotal_a = nest->htotal_a[lev];        htotal_d = nest->htotal_d[lev];
	fluxm_a  = nest->fluxm_a[lev];         fluxm_d  = nest->fluxm_d[lev];
	fluxn_a  = nest->fluxn_a[lev];         fluxn_d  = nest->fluxn_d[lev];
	r4n      = nest->r4n[lev];

	dtdx = dt / hdr.x_inc;
	dtdy = dt / hdr.y_inc;

	/* fixes the width of the lateral buffer for linear aproximation */
	/* if jupe>nnx/2 and jupe>nny/2 linear model will be applied */
	if (lev > 0) {			/* We are in a call to a nested grid */
		jupe = 0;    first = 1;    last = 0;
	}
	else {
		jupe = 5;    first = 0;    last = 1;
	}

	if (nest->do_linear) jupe = 1e6;		/* A tricky way of imposing linearity */

	/* fixes friction parameter */
	cte = (manning != 0) ? manning * manning * dt * 4.9 : 0;

	memset(fluxn_d, 0, hdr.nm * sizeof(double));	/* Do this rather than seting to zero under looping conditions */ 

	/* main computation cycle fluxn_d */
	for (row = first; row < hdr.ny - 1; row++) {
		rp1 = hdr.nx;
		rp2 = (row < hdr.ny - 2) ? 2*hdr.nx : hdr.nx;
		rm1 = (row == 0) ? 0 : hdr.nx;
		ij = row * hdr.nx - 1;
		for (col = 0; col < hdr.nx - last; col++) {
			cp1 = (col < hdr.nx - 1) ? 1 : 0;
			cm1 = (col == 0) ? 0 : 1;
			ij++;
			/* no flux to permanent dry areas */
			if (bat[ij] <= MAXRUNUP) continue;

			/* Looks weird but it's faster than an IF case (branch prediction?) */
			dqa_ij = (dqa_ij = (htotal_d[ij] + htotal_a[ij] + htotal_d[ij+rp1] + htotal_a[ij+rp1]) * 0.25) > EPS5 ? dqa_ij : 0;
			xq = 0;

			/* moving boundary - Imamura algorithm following cho 2009 */
			valid_vel = TRUE;
			if (htotal_d[ij] > EPS5 && htotal_d[ij+rp1] > EPS5) {
				if (-bat[ij+rp1] >= etad[ij]) {			/* case b2 */
					df = dd = htotal_d[ij+rp1];
					valid_vel = FALSE;		/* It means we won't believe in the velocity estimated for this cell */
				} 
				else if (-bat[ij] >= etad[ij+rp1]) {	/* case d2 */
					df = dd = htotal_d[ij];
					valid_vel = FALSE;
				}
				else {						/* case b3/d3 */
					dd = (htotal_d[ij] + htotal_d[ij+rp1]) * 0.5;
					if (dd < EPS5) dd = 0;
					df = dqa_ij;
				}
			}		 
			else if (htotal_d[ij] > EPS5 && htotal_d[ij+rp1] < EPS5 && etad[ij] > etad[ij+rp1]) {	/* case a3 and d1 wet dry */
				if (bat[ij] > bat[ij+rp1])
					df = dd = etad[ij] - etad[ij+rp1];
				else
					df = dd = htotal_d[ij];
			}
			else if (htotal_d[ij] < EPS5 && htotal_d[ij+rp1] > EPS5 && etad[ij+rp1] > etad[ij]) {	/* case b1 and c3 dry-wet */
				if (bat[ij] > bat[ij+rp1])
					df = dd = htotal_d[ij+rp1];
				else
					df = dd = etad[ij+rp1] - etad[ij];
			}
			else            /* other cases no moving boundary */
				goto L201;  /* Do this instead of a 'continue' to avoid another IF branch to account for velocity */ 

			/* disregards fluxes when dd is very small */
			if (dd < EPS4) goto L201;

			if (df < EPS4) df = EPS4;
			xpp = (fluxm_a[ij] + fluxm_a[ij+rp1] + fluxm_a[ij-cm1] + fluxm_a[ij-cm1+rp1]) * 0.25;
			ff = (manning != 0 && bat[ij] < nest->manning_depth) ? cte * sqrt(fluxn_a[ij] * fluxn_a[ij] + xpp * xpp) / pow(df, 2.333333) : 0;

			/* computes linear terms of N in cartesian coordinates */
			xq = (1 - ff) * fluxn_a[ij] - dtdy * NORMAL_GRAV * dd * (etad[ij+rp1] - etad[ij]);

			/* - if requested computes coriolis term */
			if (nest->do_Coriolis)
				xq -= r4n[row] * 2 * xpp;

			/* - total water depth is smaller than EPS3 >> linear */
			if (dqa_ij < EPS4) goto L200;
			/* - lateral buffer >> linear */
			if (col < jupe || col > (hdr.nx - jupe - 1) || row < jupe || row > (hdr.ny - jupe - 1))
				goto L200;

			/* - computes convection terms */
			advx = advy = 0;
			/* - upwind scheme for y-direction volume flux */
			/* - total water depth is smaller than EPS6 >> linear */
			if (fluxn_a[ij] < 0) {
				dqa_ij_rp1 = (htotal_d[ij+rp1] + htotal_a[ij+rp1] + htotal_d[ij+rp2] + htotal_a[ij+rp2]) * 0.25;
				if (dqa_ij_rp1 < EPS5 || htotal_d[ij+rp1] < EPS5)
					advy = -dtdy * ((fluxn_a[ij] * fluxn_a[ij]) / dqa_ij);
				else
					advy = dtdy * (fluxn_a[ij+rp1]*fluxn_a[ij+rp1] / dqa_ij_rp1 - fluxn_a[ij]*fluxn_a[ij] / dqa_ij);
			}
			else {
				dqa_ij_rm1 = (htotal_d[ij-rm1] + htotal_a[ij-rm1] + htotal_d[ij] + htotal_a[ij]) * 0.25;
				if (dqa_ij_rm1 < EPS3 || htotal_d[ij] < EPS5)
					advy = dtdy * (fluxn_a[ij] * fluxn_a[ij]) / dqa_ij;
				else
					advy = dtdy * (fluxn_a[ij] * fluxn_a[ij] / dqa_ij - fluxn_a[ij-rm1]*fluxn_a[ij-rm1] / dqa_ij_rm1);
			}
			/* - upwind scheme for x-direction volume flux */
			if (xpp < 0) {
				if (htotal_d[ij+cp1] < EPS5 || htotal_d[ij+cp1+rp1] < EPS5)
					advx = -dtdx * (fluxn_a[ij] * xpp / dqa_ij);

				else {
					dqa_ij_cp1 = (htotal_d[ij+cp1] + htotal_a[ij+cp1] + htotal_d[ij+rp1+cp1] + htotal_a[ij+rp1+cp1]) * 0.25;
					if (dqa_ij_cp1 < EPS3)
						advx = -dtdx * (fluxn_a[ij] * xpp / dqa_ij);

					else {
						xpe = (fluxm_a[ij+cp1] + fluxm_a[ij+cp1+rp1] + fluxm_a[ij] + fluxm_a[ij+rp1]) * 0.25;
						advx = dtdx * (fluxn_a[ij+cp1] * xpe / dqa_ij_cp1 - fluxn_a[ij] * xpp / dqa_ij);
					}
				}
			}
			else {
				if (htotal_d[ij-cm1] < EPS5 || htotal_d[ij-cm1+rp1] < EPS5)
					advx = dtdx * (fluxn_a[ij] * xpp / dqa_ij);

				else {
					dqa_ij_cm1 = (htotal_d[ij-cm1] + htotal_a[ij-cm1] + htotal_d[ij+rp1-cm1] + htotal_a[ij+rp1-cm1]) * 0.25;
					if (dqa_ij_cm1 < EPS3)
						advx = dtdx * (fluxn_a[ij] * xpp / dqa_ij);

					else {
						cm2 = (col < 2) ? 0 : 2;
						xpe = (fluxm_a[ij-cm1] + fluxm_a[ij-cm1+rp1] + fluxm_a[ij-cm2] + fluxm_a[ij-cm2+rp1]) * 0.25;
						advx = dtdx * (fluxn_a[ij] * xpp / dqa_ij - fluxn_a[ij-cm1] * xpe / dqa_ij_cm1);
					}
				}
			}

			/* adds linear+convection terms */
			xq = xq - advx - advy;
L200:
			xq /= (ff + 1);
#ifdef LIMIT_DISCHARGE
			if (fabs(xq) < EPS10)
				xq = 0;
			else {			/* Limit the discharge */
				f_limit = V_LIMIT * dd;
				if (xq > f_limit) xq = f_limit;
				else if (xq < -f_limit) xq = -f_limit;
			}
#endif
			fluxn_d[ij] = xq;

L201:
			if (nest->out_velocity_y && (lev == nest->writeLevel))
				vey[ij] = (valid_vel && dd > EPS3) ? xq / df : 0;
		}
	}
}

/* -------------------------------------------------------------------- */
/* initializes parameters needed for spherical computations */
/* -------------------------------------------------------------------- */
void inisp(struct nestContainer *nest) {
	int row, k = 0;
	double phim_rad, phin_rad, omega, raio_t, dxtemp, dytemp, dt;

	raio_t = 6.371e6;
	omega = 7.2722e-5;
	while (nest->level[k] >= 0) {
		dt = nest->dt[k];
		dxtemp = raio_t * nest->hdr[k].x_inc * D2R;
		dytemp = raio_t * nest->hdr[k].y_inc * D2R;
		for (row = 0; row < nest->hdr[k].ny; row++) {
			phim_rad = (nest->hdr[k].y_min + row * nest->hdr[k].y_inc) * D2R;
			phin_rad = (nest->hdr[k].y_min + (row + 0.5) * nest->hdr[k].y_inc) * D2R;
			nest->r0[k][row] = dt / dytemp;
			nest->r1m[k][row] = sin(phim_rad);
			nest->r1n[k][row] = cos(phin_rad);
			nest->r2m[k][row] = dt / dxtemp / cos(phim_rad);
			nest->r2n[k][row] = dt / dytemp / cos(phin_rad);
			nest->r3m[k][row] = NORMAL_GRAV * (dt / dxtemp) / cos(phim_rad);
			nest->r3n[k][row] = NORMAL_GRAV * (dt / dytemp);
			nest->r4m[k][row] = dt * omega * sin(phim_rad);
			nest->r4n[k][row] = dt * omega * sin(phin_rad);
		}
		k++;
	}
}

/* -------------------------------------------------------------------- */
/* initializes vectors needed for computing the Coriolis  */
/* -------------------------------------------------------------------- */
void inicart(struct nestContainer *nest) {
	int row, k = 0;
	double phim_rad, phin_rad, dt, omega = 7.2722e-5;

	while (nest->level[k] >= 0) {
		dt = nest->dt[k];
		for (row = 0; row < nest->hdr[k].ny; row++) {
			phim_rad = nest->lat_min4Coriolis + row * nest->hdr[k].y_inc * M_PI / 2e9;
			phin_rad = nest->lat_min4Coriolis + (row * nest->hdr[k].y_inc + nest->hdr[k].y_inc / 2.) * M_PI / 2e9;
			nest->r4m[k][row] = dt * omega * sin(phim_rad);
			nest->r4n[k][row] = dt * omega * sin(phin_rad);
		}
		k++;
	}
}

/* -------------------------------------------------------------------- */
/* solves non linear continuity equation w/ spherical coordinates */
/* Computes water depth (htotal) needed for friction and */
/* moving boundary algorithm */
/* htotal < 0 - dry cell htotal (m) above still water */
/* htotal > 0 - wet cell with htotal (m) of water depth */
/* -------------------------------------------------------------------- */
void mass_sp(struct nestContainer *nest, int lev) {

	unsigned int ij;
	int row, col;
	int cm1, rm1, rowm1;			/* previous column (cm1 = col -1) and row (rm1 = row - 1) */
	double etan, dd;

	for (row = 0; row < nest->hdr[lev].ny; row++) {
		ij = row * nest->hdr[lev].nx;
		rm1 = ((row == 0) ? 0 : 1) * nest->hdr[lev].nx;
		rowm1 = MAX(row - 1, 0);
		for (col = 0; col < nest->hdr[lev].nx; col++) {
			/* case ocean and non permanent dry area */
			if (nest->bat[lev][ij] > MAXRUNUP) {
				cm1 = (col == 0) ? 0 : 1;
				etan = nest->etaa[lev][ij] - nest->r2m[lev][row] * (nest->fluxm_a[lev][ij] - nest->fluxm_a[lev][ij-cm1])
				     - nest->r2n[lev][row] * (nest->fluxn_a[lev][ij] * nest->r1n[lev][row]
				     - nest->fluxn_a[lev][ij-rm1] * nest->r1n[lev][rowm1]);
				if (fabs(etan) < EPS10) etan = 0;
				dd = etan + nest->bat[lev][ij];

				/* wetable zone */
				if (dd >= EPS10) {
					nest->htotal_d[lev][ij] = dd;
					nest->etad[lev][ij] = etan;
				}
				else {		/* over dry areas htotal is null and eta follows bat */
					nest->htotal_d[lev][ij] = 0;
					nest->etad[lev][ij] = -nest->bat[lev][ij];
				}

				if (nest->do_long_beach  && (lev == nest->writeLevel) && nest->bat[lev][ij] > 0 && dd < EPS1)
					nest->long_beach[lev][ij] = 1;
				if (nest->do_short_beach && (lev == nest->writeLevel) && nest->bat[lev][ij] < 0 && dd > EPS1)
					nest->short_beach[lev][ij] = 1;
			}
			else {			/* over dry areas htotal is null and eta follows bat */
				nest->etad[lev][ij] = -nest->bat[lev][ij];
			}
			ij++;
		}
	}
}

/* ---------------------------------------------------------------------- */
/* Solve nonlinear momentum equation, in spherical coordinates */
/* with moving boundary */
/* ---------------------------------------------------------------------- */
void moment_sp_M(struct nestContainer *nest, int lev) {

	unsigned int ij;
	int first, last, jupe, row, col;
	int valid_vel;
	int cm1, rm1;			/* previous column (cm1 = col - 1) and row (rm1 = row - 1) */
	int cp1, rp1;			/* next column (cp1 = col + 1) and row (rp1 = row + 1) */
	int rm2, cp2;
	double ff = 0, cte;
	double dd, df, xp, xqe, xqq, advx, advy, f_limit;
	double dpa_ij, dpa_ij_rp1, dpa_ij_rm1, dpa_ij_cm1, dpa_ij_cp1;
	double dt, manning, *htotal_a, *htotal_d, *bat, *etad, *fluxm_a, *fluxn_a, *fluxm_d, *fluxn_d, *vex;
	double *r0, *r2m, *r3m, *r4m;
	struct grd_header hdr;
	double bat__ij;
	double htotal_d__ij;
	double htotal_d__ij_p_cp1;
	double etad__ij;
	double fluxm_a__ij;

	hdr      = nest->hdr[lev];             vex      = nest->vex[lev];
	dt       = nest->dt[lev];              manning  = nest->manning[lev];
	bat      = nest->bat[lev];             etad     = nest->etad[lev];
	htotal_a = nest->htotal_a[lev];        htotal_d = nest->htotal_d[lev];
	fluxm_a  = nest->fluxm_a[lev];         fluxm_d  = nest->fluxm_d[lev];
	fluxn_a  = nest->fluxn_a[lev];         fluxn_d  = nest->fluxn_d[lev];
	r0       = nest->r0[lev];              r2m      = nest->r2m[lev];
	r3m      = nest->r3m[lev];             r4m      = nest->r4m[lev];

	/* - fixes the width of the lateral buffer for linear aproximation */
	/* - if jupe>nnx/2 and jupe>nny/2 linear model will be applied */
	if (lev > 0) {
		jupe = 0;    first = 1;    last = 0;
	}
	else {
		jupe = 10;   first = 0;    last = 1;
	}

	if (nest->do_linear) jupe = 1e6;		/* A tricky way of imposing linearity */

	/* - fixes friction parameter */
	cte = (manning != 0) ? manning * manning * dt * 4.9 : 0;

	memset(fluxm_d, 0, hdr.nm * sizeof(double));	/* Do this rather than seting to zero under looping conditions */

	for (row = 0; row < hdr.ny - last; row++) {		/* - main computation cycle fluxm_d */
		rp1 = (row < hdr.ny - 1) ? hdr.nx : 0;
		rm1 = (row == 0) ? 0 : hdr.nx;
		ij = row * hdr.nx - 1 + first;
		for (col = first; col < hdr.nx - 1; col++) {
			cp1 = 1;
			cp2 = (col < hdr.nx - 2) ? 2 : 1;
			cm1 = (col == 0) ? 0 : 1;
			ij++;

			bat__ij = bat[ij];

			/* no flux to permanent dry areas */
			if (bat__ij <= MAXRUNUP) continue;

			htotal_d__ij = htotal_d[ij];
			htotal_d__ij_p_cp1 = htotal_d[ij+cp1];
			etad__ij = etad[ij];
			fluxm_a__ij = fluxm_a[ij];
			xp = 0;

			/* Looks weird but it's faster than an IF case (branch prediction?) */
			dpa_ij = (dpa_ij = (htotal_d__ij + htotal_a[ij] + htotal_d__ij_p_cp1 + htotal_a[ij+cp1]) * 0.25) > EPS5 ? dpa_ij : 0;

			/* - moving boundary - Imamura algorithm following cho 2009 */
			valid_vel = TRUE;
			if (htotal_d__ij > EPS5 && htotal_d__ij_p_cp1 > EPS5) {
				if (-bat[ij+cp1] >= etad__ij) {			/* - case b2 */
					df = dd = htotal_d__ij_p_cp1;
					valid_vel = FALSE;		/* It means we won't believe in the velocity estimated for this cell */
				}
				else if (-bat__ij >= etad[ij+cp1]) {	/* case d2 */
					df = dd = htotal_d__ij;
					valid_vel = FALSE;
				}
				else {			/* case b3/d3 */
					dd = (htotal_d__ij + htotal_d__ij_p_cp1) * 0.5;
					if (dd < EPS5) dd = 0;
					df = dpa_ij;
				}
			}
			else if (htotal_d__ij >= EPS5 && htotal_d__ij_p_cp1 < EPS5 && etad__ij > etad[ij+cp1]) {	/* - case a3/d1 wet dry */
				if (bat__ij > bat[ij+cp1])
					df = dd = etad__ij - etad[ij+cp1];
				else
					df = dd = htotal_d__ij;
			}
			else if (htotal_d__ij < EPS5 && htotal_d__ij_p_cp1 >= EPS5 && etad__ij < etad[ij+cp1]) {	/* - case b1 and c3 dry-wet */
				if (bat__ij > bat[ij+cp1])
					df = dd = htotal_d__ij_p_cp1;
				else
					df = dd = etad[ij+cp1] - etad__ij;
			}
			else		/* - other cases no moving boundary a1,a2,c1,c2 */
				goto L121;  /* Do this instead of a 'continue' to avoid another IF branch to account for velocity */ 

			/* - no flux if dd too small */
			if (dd < EPS5) goto L121;

			df = (df < EPS3) ? EPS3 : df;		/* Aparently this is faster than the simpe if test */
			xqq = (fluxn_a[ij] + fluxn_a[ij+cp1] + fluxn_a[ij-rm1] + fluxn_a[ij+cp1-rm1]) * 0.25;
			ff = (manning != 0) ? cte * sqrt(fluxm_a__ij * fluxm_a__ij + xqq * xqq) / pow(df, 2.333333) : 0;

			/* - computes linear terms in spherical coordinates */
			xp = (1 - ff) * fluxm_a__ij - r3m[row] * dd * (etad[ij+cp1] - etad__ij); /* - includes coriolis */

			if (nest->do_Coriolis)
				xp += r4m[row] * 2 * xqq;

			/* - total water depth is smaller than EPS3 >> linear */
			if (dpa_ij < EPS3)
				goto L120;
			/* - lateral buffer >> linear */
			if (col < jupe || col > (hdr.nx - jupe - 1) || row < jupe || row > (hdr.ny - jupe -1 ))
				goto L120;

			/* - computes convection terms */
			advx = advy = 0;
			/* - upwind scheme for x-direction volume flux */
			if (fluxm_a__ij < 0) {
				dpa_ij_cp1 = (htotal_d__ij_p_cp1 + htotal_a[ij+cp1] + htotal_d[ij+cp2] + htotal_a[ij+cp2]) * 0.25;
				/*advx = -r2m[row] * (fluxm_a__ij * fluxm_a__ij) / dpa_ij;
				if (!(dpa_ij_cp1 < EPS3 || htotal_d__ij_p_cp1 < EPS5))
					advx += r2m[row] * (fluxm_a[ij+cp1]*fluxm_a[ij+cp1]) / dpa_ij_cp1;*/

				if (dpa_ij_cp1 < EPS3 || htotal_d__ij_p_cp1 < EPS5)
					advx = -r2m[row] * (fluxm_a__ij * fluxm_a__ij) / dpa_ij;

				else
					advx = -r2m[row] * (fluxm_a__ij*fluxm_a__ij) / dpa_ij + r2m[row] * (fluxm_a[ij+cp1]*fluxm_a[ij+cp1]) / dpa_ij_cp1;
			}
			else {
				dpa_ij_cm1 = (htotal_d[ij-cm1] + htotal_a[ij-cm1] + htotal_d__ij + htotal_a[ij]) * 0.25;
				/*advx = r2m[row] * (fluxm_a__ij * fluxm_a__ij) / dpa_ij;
				if (!(dpa_ij_cm1 < EPS3 || htotal_d__ij < EPS5))
					advx -= r2m[row] * (fluxm_a[ij-cm1] * fluxm_a[ij-cm1]) / dpa_ij_cm1;*/

				if (dpa_ij_cm1 < EPS3 || htotal_d__ij < EPS5)
					advx = r2m[row] * (fluxm_a__ij * fluxm_a__ij) / dpa_ij;

				else
					advx = r2m[row] * (fluxm_a__ij * fluxm_a__ij) / dpa_ij - 
						r2m[row] * (fluxm_a[ij-cm1] * fluxm_a[ij-cm1]) / dpa_ij_cm1;
			}
			/* - upwind scheme for y-direction volume flux */
			if (xqq < 0) {
				double htotal_d__ij_p_rp1 = htotal_d[ij+rp1];
				double htotal_d__ij_p_cp1_p_rp1 = htotal_d[ij+cp1+rp1];
				dpa_ij_rp1 = (htotal_d__ij_p_rp1 + htotal_a[ij+rp1] + htotal_d__ij_p_cp1_p_rp1 + htotal_a[ij+cp1+rp1]) * 0.25;
				if (dpa_ij_rp1 < EPS5 || htotal_d__ij_p_rp1 < EPS5 || htotal_d__ij_p_cp1_p_rp1 < EPS5)
					advy = -r0[row] * (fluxm_a__ij * xqq / dpa_ij);

				else {
					xqe = (fluxn_a[ij+rp1] + fluxn_a[ij+cp1+rp1] + fluxn_a[ij] + fluxn_a[ij+cp1]) * 0.25;
					advy = -r0[row] * (fluxm_a__ij * xqq / dpa_ij) + r0[row] * (fluxm_a[ij+rp1] * xqe / dpa_ij_rp1);
				}
			} 
			else {
				double htotal_d__ij_m_rm1  = htotal_d[ij-rm1];
				double htotal_d__ij_p_cp1_m_rm1 = htotal_d[ij+cp1-rm1];
				dpa_ij_rm1 = (htotal_d__ij_m_rm1 + htotal_a[ij-rm1] + htotal_d__ij_p_cp1_m_rm1 + htotal_a[ij+cp1-rm1]) * 0.25;
				if (dpa_ij_rm1 < EPS5 || htotal_d__ij_m_rm1 < EPS5 || htotal_d__ij_p_cp1_m_rm1 < EPS5)
					advy = r0[row] * (fluxm_a__ij * xqq / dpa_ij);

				else {
					rm2 = ((row < 2) ? 0 : 2) * hdr.nx;
					xqe = (fluxn_a[ij-rm1] + fluxn_a[ij+cp1-rm1] + fluxn_a[ij-rm2] + fluxn_a[ij+cp1-rm2]) * 0.25;
					advy = r0[row] * (fluxm_a__ij * xqq / dpa_ij) - r0[row] * (fluxm_a[ij-rm1] * xqe / dpa_ij_rm1);
				}
			}
			/* - disregards very small advection terms */
			//if (fabs(advx) < EPS10) advx = 0;
			//if (fabs(advy) < EPS10) advy = 0;
			/* adds linear+convection terms */
			xp = xp - advx - advy;
L120:
			xp /= (ff + 1);
#ifdef LIMIT_DISCHARGE
			if (fabs(xp) < EPS10) xp = 0;		/* Limit the discharge */
			else {
				f_limit = V_LIMIT * dd;
				if (xp > f_limit) xp = f_limit;
				else if (xp < -f_limit) xp = -f_limit;
			}
#endif

			fluxm_d[ij] = xp;
L121:
			if (nest->out_velocity_x && (lev == nest->writeLevel))
				vex[ij] = (valid_vel && dd > EPS3) ? xp / df : 0;
		}
	}
}


/* ----------------------------------------------------------------------------------------- */
void moment_sp_N(struct nestContainer *nest, int lev) {

	unsigned int ij;
	int first, last, jupe, row, col;
	int valid_vel;
	int cm1, rm1;			/* previous column (cm1 = col - 1) and row (rm1 = row - 1) */
	int cp1, rp1;			/* next column (cp1 = col + 1) and row (rp1 = row + 1) */
	int cm2, rp2;
	double ff = 0, cte;
	double dd, df, xq, xpe, xpp, advx, advy, f_limit;
	double dqa_ij, dqa_ij_rp1, dqa_ij_rm1, dqa_ij_cm1, dqa_ij_cp1;
	double dt, manning, *htotal_a, *htotal_d, *bat, *etad, *fluxm_a, *fluxn_a, *fluxm_d, *fluxn_d, *vey;
	double *r0, *r2n, *r3n, *r4n;
	struct grd_header hdr;
	double bat__ij;
	double htotal_d__ij;
	double htotal_d__ij_p_rp1;
	double htotal_a__ij_p_rp1;
	double etad__ij;
	double etad__ij_p_rp1;
	double fluxn_a__ij;

	hdr      = nest->hdr[lev];             vey      = nest->vey[lev];
	dt       = nest->dt[lev];              manning  = nest->manning[lev];
	bat      = nest->bat[lev];             etad     = nest->etad[lev];
	htotal_a = nest->htotal_a[lev];        htotal_d = nest->htotal_d[lev];
	fluxm_a  = nest->fluxm_a[lev];         fluxm_d  = nest->fluxm_d[lev];
	fluxn_a  = nest->fluxn_a[lev];         fluxn_d  = nest->fluxn_d[lev];
	r0       = nest->r0[lev];              r2n      = nest->r2n[lev];
	r3n      = nest->r3n[lev];             r4n      = nest->r4n[lev];

	/* - fixes the width of the lateral buffer for linear aproximation */
	/* - if jupe>nnx/2 and jupe>nny/2 linear model will be applied */
	if (lev > 0) {			/* We are in a call to a nested grid */
		jupe = 0;    first = 1;    last = 0;
	}
	else {
		jupe = 10;   first = 0;    last = 1;
	}

	if (nest->do_linear) jupe = 1e6;		/* A tricky way of imposing linearity */

	/* - fixes friction parameter */
	cte = (manning != 0) ? manning * manning * dt * 4.9 : 0;

	memset(fluxn_d, 0, hdr.nm * sizeof(double));	/* Do this rather than seting to zero under looping conditions */

	/* - main computation cycle fluxn_d */
	for (row = first; row < hdr.ny - 1; row++) {
		rp1 = hdr.nx;
		rp2 = (row < hdr.ny - 2) ? 2*hdr.nx : hdr.nx;
		rm1 = (row == 0) ? 0 : hdr.nx;
		ij = row * hdr.nx - 1;
		for (col = 0; col < hdr.nx - last; col++) {
			cp1 = (col < hdr.nx-1) ? 1 : 0;
			cm1 = (col == 0) ? 0 : 1;
			ij++;

			bat__ij = bat[ij];

			/* no flux to permanent dry areas */
			if (bat__ij <= MAXRUNUP) continue;

			/* Access these the minimum times possible */
			htotal_d__ij = htotal_d[ij];
			htotal_d__ij_p_rp1 = htotal_d[ij+rp1];
			etad__ij = etad[ij];
			htotal_a__ij_p_rp1 = htotal_a[ij+rp1];
			etad__ij_p_rp1 = etad[ij+rp1];
			fluxn_a__ij = fluxn_a[ij];
			xq = 0;

			/* Looks weird but it's faster than an IF case (branch prediction?) */
			dqa_ij = (dqa_ij = (htotal_d__ij + htotal_a[ij] + htotal_d__ij_p_rp1 + htotal_a__ij_p_rp1) * 0.25) > EPS5 ? dqa_ij : 0;

			/* - moving boundary - Imamura algorithm following cho 2009 */
			valid_vel = TRUE;
			if (htotal_d__ij > EPS5 && htotal_d__ij_p_rp1 > EPS5) {
				if (-bat[ij+rp1] >= etad__ij) {				/* - case b2 */
					df = dd = htotal_d__ij_p_rp1;
					valid_vel = FALSE;		/* It means we won't believe in the velocity estimated for this cell */
				}
				else if (-bat__ij >= etad__ij_p_rp1) {		/* case d2 */
					df = dd = htotal_d__ij;
					valid_vel = FALSE;
				} 
				else {	/* case b3/d3 */
					dd = (htotal_d__ij + htotal_d__ij_p_rp1) * 0.5;
					if (dd < EPS5) dd = 0;
					df = dqa_ij;
				}
			}
			else if (htotal_d__ij > EPS5 && htotal_d__ij_p_rp1 <= EPS5 && etad__ij > etad__ij_p_rp1) {	/* - case a3 and d1 wet dry */
				if (bat__ij > bat[ij+rp1]) {
					df = dd = etad__ij - etad__ij_p_rp1;
				}
				else {
					df = dd = htotal_d__ij;
				}
			}
			else if (htotal_d__ij <= EPS5 && htotal_d__ij_p_rp1 > EPS5 && etad__ij < etad__ij_p_rp1) {	/* - case b1 and c3 dry-wet */
				if (bat__ij > bat[ij+rp1]) {
					df = dd = htotal_d__ij_p_rp1;
				}
				else {
					df = dd = etad__ij_p_rp1 - etad__ij;
				}
			} 
			else            /* - other cases no moving boundary */
				goto L201;  /* Do this instead of a 'continue' to avoid another IF branch to account for velocity */ 

			/* - no flux if dd too small */
			if (dd < EPS5) goto L201;

			df = (df < EPS3) ? EPS3 : df;
			xpp = (fluxm_a[ij] + fluxm_a[ij+rp1] + fluxm_a[ij-cm1] + fluxm_a[ij-cm1+rp1]) * 0.25;
			ff = (manning != 0) ? cte * sqrt(fluxn_a__ij * fluxn_a__ij + xpp * xpp) / pow(df, 2.333333) : 0;

			/* - computes linear terms of N in cartesian coordinates */
			xq = (1 - ff) * fluxn_a__ij - r3n[row] * dd * (etad__ij_p_rp1 - etad__ij);

			if (nest->do_Coriolis)			/* - includes coriolis effect */
				xq -= r4n[row] * 2 * xpp;

			/* - lateral buffer >> linear */
			if (col < jupe || col > (hdr.nx - jupe - 1) || row < jupe || row > (hdr.ny - jupe - 1))
				goto L200;
			/* - total water depth is smaller than EPS3 >> linear */
			if (dqa_ij < EPS3)
				goto L200;

			/* - computes convection terms */
			advx = advy = 0;
			/* - upwind scheme for y-direction volume flux */
			/* - total water depth is smaller than EPS5 >> linear */
			if (fluxn_a__ij < 0) {
				dqa_ij_rp1 = (htotal_d__ij_p_rp1 + htotal_a__ij_p_rp1 + htotal_d[ij+rp2] + htotal_a[ij+rp2]) * 0.25;
				/*advy = -r0[row] * (fluxn_a__ij * fluxn_a__ij) / dqa_ij;
				if (!(dqa_ij_rp1 < EPS5 || htotal_d__ij_p_rp1 < EPS5))
					advy += r0[row] * (fluxn_a[ij+rp1] * fluxn_a[ij+rp1]) / dqa_ij_rp1;*/

				if (dqa_ij_rp1 < EPS5 || htotal_d__ij_p_rp1 < EPS5)
					advy = -r0[row] * (fluxn_a__ij * fluxn_a__ij) / dqa_ij;
				else
					advy = r0[row] * (fluxn_a[ij+rp1]*fluxn_a[ij+rp1] / dqa_ij_rp1 - fluxn_a__ij * fluxn_a__ij / dqa_ij);
			} 
			else {
				dqa_ij_rm1 = (htotal_d[ij-rm1] + htotal_a[ij-rm1] + htotal_d__ij + htotal_a[ij]) * 0.25;
				/*advy = r0[row] * (fluxn_a__ij * fluxn_a__ij) / dqa_ij;
				if (!(dqa_ij_rm1 < EPS3 || htotal_d__ij < EPS5))
					advy -= r0[row] * (fluxn_a[ij-rm1] * fluxn_a[ij-rm1]) / dqa_ij_rm1;*/

				if (dqa_ij_rm1 < EPS3 || htotal_d__ij < EPS5)
					advy = r0[row] * (fluxn_a__ij * fluxn_a__ij) / dqa_ij;
				else
					advy = r0[row] * (fluxn_a__ij * fluxn_a__ij / dqa_ij) - r0[row] *
						(fluxn_a[ij-rm1] * fluxn_a[ij-rm1] / dqa_ij_rm1);
			}
			/* - upwind scheme for x-direction volume flux */
			if (xpp < 0) {
				double htotal_d__ij_p_cp1 = htotal_d[ij+cp1];
				dqa_ij_cp1 = (htotal_d__ij_p_cp1 + htotal_a[ij+cp1] + htotal_d[ij+rp1+cp1] + htotal_a[ij+rp1+cp1]) * 0.25;
				if (dqa_ij_cp1 < EPS3 || htotal_d__ij_p_cp1 < EPS5 || htotal_d[ij+cp1+rp1] < EPS5)
					advx = -r2n[row] * (fluxn_a__ij * xpp / dqa_ij);

				else {
					xpe = (fluxm_a[ij+cp1] + fluxm_a[ij+cp1+rp1] + fluxm_a[ij] + fluxm_a[ij+rp1]) * 0.25;
					advx = -r2n[row] * (fluxn_a__ij * xpp / dqa_ij) + r2n[row] * (fluxn_a[ij+cp1] * xpe / dqa_ij_cp1);
				}
			} 
			else {
				double htotal_d__ij_m_cm1 = htotal_d[ij-cm1];
				dqa_ij_cm1 = (htotal_d__ij_m_cm1 + htotal_a[ij-cm1] + htotal_d[ij+rp1-cm1] + htotal_a[ij+rp1-cm1]) * 0.25;
				if (dqa_ij_cm1 < EPS3 || htotal_d__ij_m_cm1 < EPS5 || htotal_d[ij-cm1+rp1] < EPS5)
					advx = r2n[row] * (fluxn_a__ij * xpp / dqa_ij);

				else {
					cm2 = (col < 2) ? 0 : 2;
					xpe = (fluxm_a[ij-cm1] + fluxm_a[ij-cm1+rp1] + fluxm_a[ij-cm2] + fluxm_a[ij-cm2+rp1]) * 0.25;
					advx = r2n[row] * (fluxn_a__ij * xpp / dqa_ij) - r2n[row] * (fluxn_a[ij-cm1] * xpe / dqa_ij_cm1);
				}
			}

			/* adds linear+convection terms */
			xq = xq - advx - advy;
L200:
			xq /= (ff + 1);
#ifdef LIMIT_DISCHARGE
			if (fabs(xq) < EPS10) xq = 0;		/* Limit the discharge */
			else {
				f_limit = V_LIMIT * dd;
				if (xq > f_limit) xq = f_limit;
				else if (xq < -f_limit) xq = -f_limit;
			}
#endif
			fluxn_d[ij] = xq;

L201:
			if (nest->out_velocity_y && (lev == nest->writeLevel))
				vey[ij] = (valid_vel && dd > EPS3) ? xq / df : 0;
		}
	}
}

/* ----------------------------------------------------------------------------------------- */
void interp_edges(struct nestContainer *nest, double *flux_L1, double *flux_L2, char *what, int lev, int i_time) {
	/* Interpolate outer Fluxes on boundary edges with the resolution of the nested grid
	   and assign them to inner grid, at its boundaries. */
	int i, n, col, row, last_iter;
	double s, t1, *bat_P, *etad_P;
	//unsigned int ij;
	//double grx, gry, c1, c2, hp, hm, xm;

	bat_P  = nest->bat[lev-1];	/* Parent bathymetry */;
	etad_P = nest->etad[lev-1];
	last_iter = (int)(nest->dt[lev-1] / nest->dt[lev]);  /* No truncations here */

	if (what[0] == 'N') {			/* Only FLUXN uses this branch */
		n = (nest->LRcol[lev] - nest->LLcol[lev] + 1);
		/* SOUTH boundary */
		s = nest->hdr[lev].y_inc / nest->hdr[lev-1].y_inc;
		for (i = 0, col = nest->LLcol[lev]; col <= nest->LRcol[lev]; col++, i++) {
			t1 = flux_L1[ij_grd(col, nest->LLrow[lev], nest->hdr[lev-1])];
			//t2 = flux_L1[ij_grd(col, nest->LLrow[lev]+1, nest->hdr[lev-1])];
			nest->edge_row_Ptmp[lev][i] = t1;
		}
		intp_lin (nest->edge_row_P[lev], nest->edge_row_Ptmp[lev], n, nest->hdr[lev].nx,
			nest->edge_row[lev], nest->edge_rowTmp[lev]);
		for (col = 0; col < nest->hdr[lev].nx; col++) {		/* Put interp val in nested grid */
			if (nest->bat[lev][ij_grd(col, 0, nest->hdr[lev])] + nest->etaa[lev][ij_grd(col, 0, nest->hdr[lev])] > EPS5)
				flux_L2[ij_grd(col, 0, nest->hdr[lev])] = nest->edge_rowTmp[lev][col];
			else
				flux_L2[ij_grd(col,0,nest->hdr[lev])] = 0;
		}

		/* NORTH boundary */
		for (i = 0, col = nest->LLcol[lev]; col <= nest->LRcol[lev]; col++, i++) {
			t1 = flux_L1[ij_grd(col, nest->ULrow[lev]-1, nest->hdr[lev-1])];
			//t2 = flux_L1[ij_grd(col, nest->ULrow[lev],   nest->hdr[lev-1])];
			nest->edge_row_Ptmp[lev][i] = t1;
		}
		intp_lin (nest->edge_row_P[lev], nest->edge_row_Ptmp[lev], n, nest->hdr[lev].nx,
			nest->edge_row[lev], nest->edge_rowTmp[lev]);
		for (col = 0; col < nest->hdr[lev].nx; col++) {
			if (nest->bat[lev][ij_grd(col, nest->hdr[lev].ny-1, nest->hdr[lev])] +
			    nest->etaa[lev][ij_grd(col, nest->hdr[lev].ny-1, nest->hdr[lev])] > EPS5)
				flux_L2[ij_grd(col, nest->hdr[lev].ny-1, nest->hdr[lev])] = nest->edge_rowTmp[lev][col];
			else
				flux_L2[ij_grd(col,nest->hdr[lev].ny-1,nest->hdr[lev])] = 0;
		}
	}
	else {							/* Only FLUXM uses this branch */
		//grx = NORMAL_GRAV * nest->dt[lev] / nest->hdr[lev].x_inc;
		n = (nest->ULrow[lev] - nest->LLrow[lev] + 1);
		/* WEST (left) boundary. */
		s = nest->hdr[lev].x_inc / nest->hdr[lev-1].x_inc;
		for (i = 0, row = nest->LLrow[lev]; row <= nest->ULrow[lev]; row++, i++) {
			t1 = flux_L1[ij_grd(nest->LLcol[lev],   row, nest->hdr[lev-1])];
			//t2 = flux_L1[ij_grd(nest->LLcol[lev]+1, row, nest->hdr[lev-1])];
			nest->edge_col_Ptmp[lev][i] = t1;
		}
		intp_lin (nest->edge_col_P[lev], nest->edge_col_Ptmp[lev], n, nest->hdr[lev].ny,
			nest->edge_col[lev], nest->edge_colTmp[lev]);
		for (row = 0; row < nest->hdr[lev].ny; row++) {		/* Put interp val in nested grid */
			if (nest->bat[lev][ij_grd(0, row, nest->hdr[lev])] + nest->etaa[lev][ij_grd(0, row, nest->hdr[lev])] > EPS5)
				flux_L2[ij_grd(0,row,nest->hdr[lev])] = nest->edge_colTmp[lev][row];
			else
				flux_L2[ij_grd(0,row,nest->hdr[lev])] = 0;
		}

		/* EAST (right) boundary */
		//if (i_time == 0) {
			for (i = 0, row = nest->LLrow[lev]; row <= nest->ULrow[lev]; row++, i++)
				nest->edge_col_Ptmp[lev][i] = flux_L1[ij_grd(nest->LRcol[lev]-1, row, nest->hdr[lev-1])];
#if 0
		}
		else {
			c1 = (double)(i_time) / (double)(last_iter);
			c2 = 1 - c1;
			for (i = 0, row = nest->LLrow[lev]; row <= nest->ULrow[lev]; row++, i++) {
				ij = ij_grd(nest->LLcol[lev], row, nest->hdr[lev-1]);
				nest->edge_col_Ptmp[lev][i] = 0;
				if ((bat_P[ij-1] + etad_P[ij-1] < EPS5) || (bat_P[ij] + etad_P[ij] < EPS5))
					continue;
				hp = 0.5 * (bat_P[ij-1] + bat_P[ij]);
				hm = hp + 0.5 * (etad_P[ij-1] + etad_P[ij]);
				xm = flux_L1[ij-1] - grx * hm * (etad_P[ij] - etad_P[ij-1]);
				if (fabs(xm) < EPS6) xm = 0;
				nest->edge_col_Ptmp[lev][i] = c1 * xm + c2 * flux_L1[ij-1];
			}
		}
#endif

		intp_lin (nest->edge_col_P[lev], nest->edge_col_Ptmp[lev], n, nest->hdr[lev].ny,
			nest->edge_col[lev], nest->edge_colTmp[lev]);
		for (row = 0; row < nest->hdr[lev].ny; row++) {
			if (nest->bat[lev][ij_grd(nest->hdr[lev].nx-1,  row, nest->hdr[lev])] +
			    nest->etaa[lev][ij_grd(nest->hdr[lev].nx-1, row, nest->hdr[lev])] > EPS5)
				flux_L2[ij_grd(nest->hdr[lev].nx-1, row, nest->hdr[lev])] = nest->edge_colTmp[lev][row];
			else
				flux_L2[ij_grd(nest->hdr[lev].nx-1, row, nest->hdr[lev])] = 0;
		}
	}
}

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
 * INTP_LIN will interpolate from the dataset <x,y> onto a new set <u,v>
 * where <x,y> and <u> is supplied by the user. <v> is returned. 
 *
 * input:  x = x-values of input data
 *	   y = y-values "    "     "
 *	   n = number of data pairs
 *	   m = number of desired interpolated values
 *	   u = x-values of these points
 * output: v = y-values at interpolated points
 *
 * Programmer:	Paul Wessel
 * Date:	16-JAN-1987
 * Ver:		v.2 --> cripled version for linear interpolation (J.L.)
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
 */

int intp_lin (double *x, double *y, int n, int m, double *u, double *v) {
	int i, j, err_flag = 0;
	int down = FALSE;
	double dx;
	
	/* Check to see if x-values are monotonically increasing/decreasing */

	dx = x[1] - x[0];
	if (dx > 0.0) {
		for (i = 2; i < n && err_flag == 0; i++)
			if ((x[i] - x[i-1]) < 0.0) err_flag = i;
	}

	else {
		down = TRUE;
		for (i = 2; i < n && err_flag == 0; i++)
			if ((x[i] - x[i-1]) > 0.0) err_flag = i;
	}

	if (err_flag) {
		mexPrintf ("%s: Fatal Error: x-values are not monotonically increasing/decreasing!\n", "intp_lin");
		return (err_flag);
	}
	
	if (down) {	/* Must flip directions temporarily */
		for (i = 0; i < n; i++) x[i] = -x[i];
		for (i = 0; i < m; i++) u[i] = -u[i];
	}

	/* Compute the interpolated values from the coefficients */
	
	j = 0;
	for (i = 0; i < m; i++) {
		while (x[j] > u[i] && j > 0) j--;	/* In case u is not sorted */
		while (j < n && x[j] <= u[i]) j++;
		if (j == n) j--;
		if (j > 0) j--;

		dx = u[i] - x[j];
		v[i] = (y[j+1]-y[j])*dx/(x[j+1]-x[j]) + y[j];
	}

	if (down) {	/* Must reverse directions */
		for (i = 0; i < n; i++) x[i] = -x[i];
		for (i = 0; i < m; i++) u[i] = -u[i];
	}

	return (0);
}

/* ------------------------------------------------------------------------------------------- */
/* upscale from doughter to parent level
/* ------------------------------------------------------------------------------------------- */
void upscale(struct nestContainer *nest, double *out, int lev, int i_tsr) {
	/* Computes the mean of cells inside a square window
	   lev   -> This grid level
	*/
	int	inc, k, col, row, col_P, row_P, wcol, wrow, prow, count, half, rim, do_half = FALSE;
	unsigned int ij, nm;
	double	*p, *pa, soma, *bat_P;

	inc = nest->incRatio[lev];	/* Grid spatial ratio between Parent and doughter */
	bat_P = nest->bat[lev-1];	/* Parent bathymetry */

	nm = nest->hdr[lev].nx * nest->hdr[lev].ny;
	for (ij = 0; ij < nm; ij++)
		if (nest->bat[lev][ij] < 0) nest->etad[lev][ij] += nest->bat[lev][ij];

	if (i_tsr % 2 == 0) do_half = TRUE;  /* Compute eta as the mean of etad & etaa */

	half = irint(floor(nest->incRatio[lev] * nest->incRatio[lev] * 2.0 / 3.0));

	rim = 1 * inc;
	for (row = 0+rim, prow = 0, row_P = nest->LLrow[lev]+1; row < nest->hdr[lev].ny-rim; row_P++, prow++, row += inc) {
		ij = ij_grd(nest->LLcol[lev] + 1,  nest->LLrow[lev] + 1 + prow,  nest->hdr[lev-1]);
		for (col = 0+rim, col_P = nest->LLcol[lev]+1; col < nest->hdr[lev].nx-rim; col_P++, col += inc) {
			k = col + row * nest->hdr[lev].nx;       /* Index of window's LL corner */
			soma = 0;
			count = 0;
			for (wrow = 0; wrow < inc; wrow++) {     /* Loop rows inside window */
				p = &nest->etad[lev][k + wrow * nest->hdr[lev].nx];
				if (do_half) pa = &nest->etaa[lev][k + wrow * nest->hdr[lev].nx];
				for (wcol = 0; wcol < inc; wcol++) {
					if (nest->bat[lev][k + wcol + wrow * nest->hdr[lev].nx] + *p > EPS5) {
						if (do_half)
							soma += (*p + *pa) * 0.5;
						else
							soma += *p;
						count++;
					}
					p++;
					if (do_half) pa++;
				}
			}
			/* --- case when more than 50% of daugther cells add to a mother cell */
			if (soma && count > half) {
				if (bat_P[ij_grd(col_P, row_P, nest->hdr[lev-1])] < 0)
					out[ij] = soma / count - bat_P[ij_grd(col_P, row_P, nest->hdr[lev-1])];
				else
					out[ij] = soma / count;
			}
			ij++;
		}
	}

	/* --- reputs bathymetry on etad on land --- */
	for (ij = 0; ij < nm; ij++)
		if (nest->bat[lev][ij] < 0) nest->etad[lev][ij] -= nest->bat[lev][ij];
}

/* --------------------------------------------------------------------- */
/* upscale from doughter to parent level
/* --------------------------------------------------------------------- */
void upscale_(struct nestContainer *nest, double *etad, int lev, int i_tsr) {
	/* i_tst -> loop variable over the time step ration of the two grids */
	int half, count, row, col, nrow, ncol, rim, do_half = FALSE;
	int i0, j0, ii, jj, ki, kj;
	unsigned int ij;
	double sum, *bat_P;

	bat_P = nest->bat[lev-1];	/* Parent bathymetry */

	if (i_tsr % 2 == 0) do_half = TRUE;  /* Compute eta as the mean of etad & etaa */

	half = irint(floor(nest->incRatio[lev] * nest->incRatio[lev] * 2.0 / 3.0));

	for (ij = 0; ij < nest->hdr[lev].nm; ij++)
		if (nest->bat[lev][ij] < 0) nest->etad[lev][ij] += nest->bat[lev][ij];

	rim = 1;
	for (row = nest->LLrow[lev] + 1 + rim, nrow = rim; row < nest->ULrow[lev] - rim; row++, nrow++) {
		i0 = nrow * nest->incRatio[lev];
		for (col = nest->LLcol[lev] + 1 + rim, ncol = rim; col < nest->LRcol[lev] - rim; col++, ncol++) {
			j0 = ncol * nest->incRatio[lev];
			sum = 0;
			count = 0;
			for (ki = 0; ki < nest->incRatio[lev]; ki++) {
				ii = i0 + ki;
				for (kj = 0; kj < nest->incRatio[lev]; kj++) {
					jj = j0 + kj;
					ij = ij_grd(jj,ii, nest->hdr[lev]);
					if (nest->bat[lev][ij] + nest->etad[lev][ij] > EPS5) {
						if (do_half)
							sum += 0.5 * (nest->etaa[lev][ij] + nest->etad[lev][ij]);
							//sum += 0.5 * (nest->etaa[lev][ij] + nest->etad[lev][ij]) + nest->bat[lev][ij];
						else
							sum += nest->etad[lev][ij];
							//sum += nest->etad[lev][ij] + nest->bat[lev][ij];
						count++;
					}
				}
			}

			/* --- case when more than 50% of daugther cells add to a mother cell */
			if (sum && count >= half) {
				//etad[ij_grd(col,row, nest->hdr[lev-1])] = sum / count - bat_P[ij_grd(col,row, nest->hdr[lev-1])];
				if (bat_P[ij_grd(col,row, nest->hdr[lev-1])] < 0)
					etad[ij_grd(col,row, nest->hdr[lev-1])] = sum / count - bat_P[ij_grd(col,row, nest->hdr[lev-1])];
				else
					etad[ij_grd(col,row, nest->hdr[lev-1])] = sum / count;
			}
		}
	}

	/* --- reputs bathymetry on etad on land --- */
	for (ij = 0; ij < nest->hdr[lev].nm; ij++)
		if (nest->bat[lev][ij] < 0) nest->etad[lev][ij] -= nest->bat[lev][ij];
}

/* --------------------------------------------------------------------- */
void replicate(struct nestContainer *nest, int lev) {
	/* Replicate Left and Bottom boundaries */
	int	col, row;

	for (row = 0; row < nest->hdr[lev].ny; row++) {
		if (nest->bat[lev][ij_grd(0, row, nest->hdr[lev])] < 0) continue;
		nest->etad[lev][ij_grd(0, row, nest->hdr[lev])] =
			nest->etad[lev][ij_grd(1, row, nest->hdr[lev])];
	}

	for (col = 0; col < nest->hdr[lev].nx; col++) {
		if (nest->bat[lev][ij_grd(col, 0, nest->hdr[lev])] < 0) continue;
		nest->etad[lev][ij_grd(col, 0, nest->hdr[lev])] =
			nest->etad[lev][ij_grd(col, 1, nest->hdr[lev])];
	}
}

/* ------------------------------------------------------------------------------ */
void nestify(struct nestContainer *nest, int nNg, int level, int isGeog) {
	/* nNg -> number of nested grids */
	/* level contains the number of times this function was called recursively.
	   Start with 0 in first call and this counter is increased internally */
	int j, last_iter, nhalf;

	if (nest->run_jump_time > 0) {      /* If holding childrens state */
		if (nest->run_jump_time > nest->time_h)
			return;
		else {
			/* At this point we must interpolate children's eta & flux to not create family discontinuities */
			resamplegrid(nest, nNg);
			nest->run_jump_time = 0;    /* Since we are done, reset to zero so we won't pass here again */
		}
	}

	last_iter = (int)(nest->dt[level-1] / nest->dt[level]);  /* No truncations here */
	nhalf = (int)((float)last_iter / 2);           /* */
	for (j = 0; j < last_iter; j++) {
		edge_communication(nest, level, j);
		mass_conservation(nest, isGeog, level);

		if (nest->do_max_level)    update_max(nest);             /* This makes sure all time steps are visited */
		if (nest->do_max_velocity) update_max_velocity(nest);    /* This makes sure all time steps are visited */

		/* MAGIC happens here */
		if (nNg != 1)
			nestify(nest, nNg - 1, level + 1, isGeog);

		moment_conservation(nest, isGeog, level);
		replicate(nest, level);

		if (j == nhalf && nest->do_upscale)           /* Do the upscale only at middle iteration of this cycle */
			upscale_(nest, nest->etad[level-1], level, last_iter);

		update(nest, level);
	}
}

/* ------------------------------------------------------------------------------ */
void resamplegrid(struct nestContainer *nest, int nNg) {
	/* interpolate children's eta & flux to not create family discontinuities */
	/* nNg -> number of nested grids */
	int row, col, k;
	size_t ij;
	double xx, yy;
	for (k = 1; k <= nNg; k++) {
		for (row = ij = 0; row < nest->hdr[k].ny; row++) {
			yy = nest->hdr[k].y_min + row * nest->hdr[k].y_inc;
			for (col = 0; col < nest->hdr[k].nx; col++, ij++) {
				if (nest->bat[k][ij] < 0) continue;
				xx = nest->hdr[k].x_min + col * nest->hdr[k].x_inc;
				nest->etaa[k][ij]    = GMT_get_bcr_z(nest->etaa[k-1],    nest->hdr[k-1], xx, yy);
				nest->etad[k][ij]    = GMT_get_bcr_z(nest->etad[k-1],    nest->hdr[k-1], xx, yy);
				nest->fluxm_a[k][ij] = GMT_get_bcr_z(nest->fluxm_a[k-1], nest->hdr[k-1], xx, yy);
				nest->fluxn_a[k][ij] = GMT_get_bcr_z(nest->fluxn_a[k-1], nest->hdr[k-1], xx, yy);
				nest->fluxm_d[k][ij] = GMT_get_bcr_z(nest->fluxm_d[k-1], nest->hdr[k-1], xx, yy);
				nest->fluxn_d[k][ij] = GMT_get_bcr_z(nest->fluxn_d[k-1], nest->hdr[k-1], xx, yy);
				nest->htotal_a[k][ij]= GMT_get_bcr_z(nest->htotal_a[k-1],nest->hdr[k-1], xx, yy);
				nest->htotal_d[k][ij]= GMT_get_bcr_z(nest->htotal_d[k-1],nest->hdr[k-1], xx, yy);
			}
		}
	}
}

/* ------------------------------------------------------------------------------ */
void edge_communication(struct nestContainer *nest, int lev, int i_time) {
	interp_edges(nest, nest->fluxm_a[lev-1], nest->fluxm_a[lev], "M", lev, i_time);
	interp_edges(nest, nest->fluxn_a[lev-1], nest->fluxn_a[lev], "N", lev, i_time);
}

/* ------------------------------------------------------------------------------ */
void mass_conservation(struct nestContainer *nest, int isGeog, int m) {
	/* m is the level of nesting which starts counting at one for FIRST nesting level */
	if (isGeog == 0)
		mass(nest, m);
	else
		mass_sp(nest, m);
}

/* ------------------------------------------------------------------------------ */
void moment_conservation(struct nestContainer *nest, int isGeog, int m) {
	/* m is the level of nesting which starts counting at one for FIRST nesting level */
	int i;

#ifdef DO_MULTI_THREAD
	HANDLE ThreadList[2];  /* Handles to the worker threads */
	ThreadArg Arg_List[2], *Arg_p;

	if (isGeog == 0) { 
		for (i = 0; i < 2; i++) {
			Arg_p           = &Arg_List[i];
			Arg_p->nest     = nest;
			Arg_p->iThread  = i;
			Arg_p->lev      = m;
			ThreadList[i] = (HANDLE)_beginthreadex(NULL, 0, MT_cart, Arg_p, 0, NULL);
		}
	}
	else {
		for (i = 0; i < 2; i++) {
			Arg_p           = &Arg_List[i];
			Arg_p->nest     = nest;
			Arg_p->iThread  = i;
			Arg_p->lev      = m;
			ThreadList[i] = (HANDLE)_beginthreadex(NULL, 0, MT_sp, Arg_p, 0, NULL);
		}
	}

	/* Wait until all threads are ready and close the handles */
	WaitForMultipleObjects(2, ThreadList, TRUE, INFINITE);
	for (i = 0; i < 2; i++)
		CloseHandle(ThreadList[i]);
#else
	if (isGeog == 0) { 
		for (i = 0; i < 2; i++)
			call_moment[i](nest, m);
	}
	else {
		for (i = 0; i < 2; i++)
			call_moment_sp[i](nest, m);
	}
#endif
}

#ifdef DO_MULTI_THREAD
/* ------------------------------------------------------------------------------ */
unsigned __stdcall MT_cart(void *Arg_p) {
	/* Convert input from (void *) to (ThreadArg *), call the moment and stop the thread. */
  
	ThreadArg *Arg = (ThreadArg *)Arg_p;
	call_moment[Arg->iThread](Arg->nest, Arg->lev);
	_endthreadex(0);
	return (0);
}
/* ------------------------------------------------------------------------------ */
unsigned __stdcall MT_sp(void *Arg_p) {
	/* Convert input from (void *) to (ThreadArg *), call the moment and stop the thread. */
  
	ThreadArg *Arg = (ThreadArg *)Arg_p;
	call_moment_sp[Arg->iThread](Arg->nest, Arg->lev);
	_endthreadex(0);
	return (0);
}

/* ------------------------------------------------------------------------------ */
int GetLocalNThread(void) {
	/* Get number of processors from the environment variable NUMBER_OF_PROCESSORS. */
  
	char *pStr;
	int   localNThread = 1;  /* Default */
  
	if ((pStr = getenv("NUMBER_OF_PROCESSORS")) != NULL)
		sscanf(pStr, "%d", &localNThread);
  
	return (localNThread);
}

#endif


/* ---------------------------------------------------------------------------------------- */
void kaba_source(struct srf_header hdr, double x_inc, double y_inc, double x_min, double x_max,
	double y_min, double y_max, int type, double *z) {
	/* Create a prismatic source (a Kaba) to use as source for the Green's functions method.
	   when type = 1, all variables represent what their names say
	   when type = 2, x_min/x_max are instead the prism's center and y_min/y_max its half widths
	*/
	int row, col, col1, col2, row1, row2;

	if (type == 1) {		/* We will select the closest nodes that are INSIDE the -R region on L & B sides and ON R & T sides */
		col1 = irint((x_min - hdr.x_min) / x_inc) + 1;		/* Leftmost column not included*/
		col2 = irint((x_max - hdr.x_min) / x_inc);
		row1 = irint((y_min - hdr.y_min) / y_inc) + 1;		/* Bottomost row not included */
		row2 = irint((y_max - hdr.y_min) / y_inc);
	}
	else {
		int nx2 = (int)y_min;
		int ny2 = (int)y_max;
		col1 = irint((x_min - hdr.x_min) / x_inc) - nx2;
		col2 = col1 + 2*nx2;
		row1 = irint((y_min - hdr.y_min) / y_inc) - ny2;
		row2 = row1 + 2*ny2;
	}
	memset(z, 0, hdr.nx * hdr.ny * sizeof(double));		/* Need because this function may be called recursivly */
	for (row = row1; row <= row2; row++) {
		for (col = col1; col <= col2; col++) {
			z[ij_grd(col,row,hdr)] = 1;
		}
	}
}

/* ---------------------------------------------------------------------------------------- */
void deform(struct srf_header hdr, double x_inc, double y_inc, int isGeog, double fault_length,
	double fault_width, double th, double dip, double rake, double d, double top_depth,
	double xl, double yl, double *z) {

	/*	Compute the vertical deformation component according to Okada formulation */

	int i, j;
	unsigned int k = 0;
	double h1, h2, ds, dd, xx, yy, x1, x2, x3, us, ud, sn_tmp, cs_tmp, tg_tmp;
	double f1, f2, f3, f4, g1, g2, g3, g4, rx, ry, lon0;
	double t_c1, t_c2, t_c3, t_c4, t_e2, t_M0, f_length2;

	/* Initialize TM variables. Fault origin will be used as projection's origin. However,
	   this would set it as a singularity point. That's why it is arbitrarely shifted
	   by a 1/4 of grid step. */ 
	if (isGeog) {
		vtm(yl + y_inc / 2, &t_c1, &t_c2, &t_c3, &t_c4, &t_e2, &t_M0);
		lon0 = xl + x_inc / 2;		/* Central meridian for this transform */
	}

	f_length2 = fault_length / 2;
	dip *= D2R;
	h1 = top_depth / sin(dip);
	h2 = top_depth / sin(dip) + fault_width;
	ds = -d * cos(D2R * rake);
	dd =  d * sin(D2R * rake);
	sn_tmp = sin(D2R*th);	cs_tmp = cos(D2R*th);	tg_tmp = tan(dip);

	for (i = 0; i < hdr.ny; i++) {
		yy = hdr.y_min + y_inc * i;
		for (j = 0; j < hdr.nx; j++) {
			xx = hdr.x_min + x_inc * j;
			if (isGeog)
				tm(xx, yy, &rx, &ry, lon0, t_c1, t_c2, t_c3, t_c4, t_e2, t_M0);	/* Remember that (xl,yl) is already the proj origin */
			else {
				rx = xx - xl;
				ry = yy - yl;
			}
			x1 = rx*sn_tmp + ry*cs_tmp - f_length2;
			x2 = rx*cs_tmp - ry*sn_tmp + top_depth/tg_tmp;
			x3 = 0.0;
			f1 = uscal(x1, x2, x3,  f_length2, h2, dip);
			f2 = uscal(x1, x2, x3,  f_length2, h1, dip);
			f3 = uscal(x1, x2, x3, -f_length2, h2, dip);
			f4 = uscal(x1, x2, x3, -f_length2, h1, dip);
			g1 = udcal(x1, x2, x3,  f_length2, h2, dip);
			g2 = udcal(x1, x2, x3,  f_length2, h1, dip);
			g3 = udcal(x1, x2, x3, -f_length2, h2, dip);
			g4 = udcal(x1, x2, x3, -f_length2, h1, dip);
			us = (f1-f2-f3+f4) * ds / (12 * M_PI);
			ud = (g1-g2-g3+g4) * dd / (12 * M_PI);
			z[k++] = us + ud;
		}
	}
}

/* ---------------------------------------------------------------------------------------- */
double uscal(double x1, double x2, double x3, double c, double cc, double dp) {
	/* Computation of the vertical displacement due to the STRIKE and SLIP component */
	double sn, cs, c1, c2, c3, r, q, r2, r3, q2, q3, h, k, a1, a2, a3, f;
	double b1, b2, b3, b4, b5, b6, b7, b8, b9, b10, b11, b12, b13, b14;

	sn  = sin(dp);	cs = cos(dp);
	c1  = c;		c2 = cc * cs;	c3 = cc * sn;
	r   = sqrt((x1-c1)*(x1-c1) + (x2-c2)*(x2-c2) + (x3-c3)*(x3-c3));
	q   = sqrt((x1-c1)*(x1-c1) + (x2-c2)*(x2-c2) + (x3+c3)*(x3+c3));
	r2  = x2*sn - x3*cs;	r3 = x2*cs + x3*sn;
	q2  = x2*sn + x3*cs;	q3 = -x2*cs + x3*sn;
	h   = sqrt(q2*q2 + (q3+cc)*(q3+cc));
	k   = sqrt(q2*q2 + (x1-c1)*(x1-c1));
	a1  = log(r+r3-cc);	a2 = log(q+q3+cc);	a3 = log(q+x3+c3);
	b1  = 1. + 3. * (tan(dp)*tan(dp));
	b2  = 3. * tan(dp) / cs;
	b3  = 2. * r2 * sn;
	b4  = q2 + x2 * sn;
	b5  = 2. * r2*r2 * cs;
	b6  = r * (r+r3-cc);
	b7  = 4. * q2 * x3 * sn*sn;
	b8  = 2. * (q2+x2*sn) * (x3+q3*sn);
	b9  = q * (q+q3+cc);
	b10 = 4. * q2 * x3 * sn;
	b11 = (x3+c3) - q3 * sn;
	b12 = 4. * q2*q2 * q3 * x3 * cs * sn;
	b13 = 2. * q + q3 + cc;
	b14 = pow(q,3) * pow((q+q3+cc),2);
	f   = cs * (a1 + b1*a2 - b2*a3) + b3/r + 2.*sn*b4/q - b5/b6 + (b7-b8)/b9 + b10*b11/(pow(q,3)) - b12*b13/b14;

	return (f);
}

/* ---------------------------------------------------------------------------------------- */
double udcal(double x1, double x2, double x3, double c, double cc, double dp) {
	/* Computation of the vertical displacement due to the DIP SLIP component */
	double sn, cs, c1, c2, c3, r, q, r2, r3, q2, q3, h, k, a1, a2;
	double b1, b2, b3, d1, d2, d3, d4, d5, d6, t1, t2, t3, f;

	sn = sin(dp);	cs = cos(dp);
	c1 = c;		c2 = cc * cs;	c3 = cc * sn;
	r = sqrt((x1-c1)*(x1-c1) + (x2-c2)*(x2-c2) + (x3-c3)*(x3-c3));
	q = sqrt((x1-c1)*(x1-c1) + (x2-c2)*(x2-c2) + (x3+c3)*(x3+c3));
	r2 = x2*sn - x3*cs;	r3 = x2*cs + x3*sn;
	q2 = x2*sn + x3*cs;	q3 = -x2*cs + x3*sn;
	h = sqrt(q2*q2 + (q3+cc)*(q3+cc));
	k = sqrt(q2*q2 + (x1-c1)*(x1-c1));
	a1 = log(r+x1-c1);	a2 = log(q+x1-c1);
	b1 = q * (q+x1-c1);	b2 = r * (r+x1-c1);	b3 = q * (q+q3+cc);
	d1 = x1 - c1;		d2 = x2 - c2;		d3 = x3 - c3;
	d4 = x3 + c3;		d5 = r3 - cc;		d6 = q3 + cc;
	t1 = atan2(d1*d2, (h+d4)*(q+h));
	t2 = atan2(d1*d5, r2*r);
	t3 = atan2(d1*d6, q2*q);
	f = sn * (d2*(2.*d3/b2 + 4.*d3/b1 - 4.*c3*x3*d4*(2.*q+d1)/(b1*b1*q)) - 6.*t1 + 3.*t2 - 6.*t3) +
	    cs * (a1-a2 - 2.*(d3*d3)/b2 - 4.*(d4*d4 - c3*x3)/b1 - 4.*c3*x3*d4*d4*(2*q+x1-c1)/(b1*b1*q)) +
	    6.*x3*(cs*sn*(2.*d6/b1 + d1/b3) - q2*(sn*sn - cs*cs)/b1);

	return (f);
}

/* ---------------------------------------------------------------------------------------- */
void vtm(double lat0, double *t_c1, double *t_c2, double *t_c3, double *t_c4, double *t_e2, double *t_M0) {
	/* Set up an TM projection (extract of GMT_vtm)*/
	double lat2, s2, c2;
	
	lat0 *= D2R;
	lat2 = 2.0 * lat0;
	s2 = sin(lat2);
	c2 = cos(lat2);
	*t_c1 = 1.0 - (1.0/4.0) * ECC2 - (3.0/64.0) * ECC4 - (5.0/256.0) * ECC6;
	*t_c2 = -((3.0/8.0) * ECC2 + (3.0/32.0) * ECC4 + (25.0/768.0) * ECC6);
	*t_c3 = (15.0/128.0) * ECC4 + (45.0/512.0) * ECC6;
	*t_c4 = -(35.0/768.0) * ECC6;
	*t_e2 = ECC2 / (1.0 - ECC2);
	*t_M0 = EQ_RAD * (*t_c1 * lat0 + s2 * (*t_c2 + c2 * (*t_c3 + c2 * *t_c4)));
}

/* ---------------------------------------------------------------------------------------- */
void tm(double lon, double lat, double *x, double *y, double central_meridian, double t_c1,
	double t_c2, double t_c3, double t_c4, double t_e2, double t_M0) {
	/* Convert lon/lat to TM x/y (adapted from GMT_tm) */
	double N, T, T2, C, A, M, dlon, tan_lat, A2, A3, A5, lat2, s, c, s2, c2;

	if (fabs (fabs (lat) - 90.0) < GMT_CONV_LIMIT) {
		M = EQ_RAD * t_c1 * M_PI_2;
		*x = 0.0;
		*y = M;
	}
	else {
		lat *= D2R;
		lat2 = 2.0 * lat;
		s = sin(lat);	s2 = sin(lat2);
		c = cos(lat);	c2 = cos(lat2);
		tan_lat = s / c;
		M = EQ_RAD * (t_c1 * lat + s2 * (t_c2 + c2 * (t_c3 + c2 * t_c4)));
		dlon = lon - central_meridian;
		if (fabs (dlon) > 360.0) dlon += Loc_copysign (360.0, -dlon);
		if (fabs (dlon) > 180.0) dlon  = Loc_copysign (360.0 - fabs (dlon), -dlon);
		N = EQ_RAD / sqrt (1.0 - ECC2 * s * s);
		T = tan_lat * tan_lat;
		T2 = T * T;
		C = t_e2 * c * c;
		A = dlon * D2R * c;
		A2 = A * A;	A3 = A2 * A;	A5 = A3 * A2;
		*x = N * (A + (1.0 - T + C) * (A3 * 0.16666666666666666667) + (5.0 - 18.0 * T + T2 + 72.0 * C - 58.0 * t_e2) *
		     (A5 * 0.00833333333333333333));
		A3 *= A;	A5 *= A;
		*y = (M - t_M0 + N * tan_lat * (0.5 * A2 + (5.0 - T + 9.0 * C + 4.0 * C * C) * (A3 * 0.04166666666666666667) +
		     (61.0 - 58.0 * T + T2 + 600.0 * C - 330.0 * t_e2) * (A5 * 0.00138888888888888889)));
	}
}

/* ---------------------------------------------------------------------------------------- */
double GMT_get_bcr_z(double *grd, struct grd_header hdr, double xx, double yy) {
	/* Given xx, yy in user's grid file (in non-normalized units)
	   this routine returns the desired bicubic interpolated value at xx, yy
	   ADAPTED from GMT's routine with the same name. */

	unsigned int i, j, ij, node;
	double retval, wsum, wx[4], wy[4], w;

	/* Determine nearest node ij and set weights wx, wy */

	ij = gmt_bcr_prep (hdr, xx, yy, wx, wy);

	retval = wsum = 0.0;
	for (j = 0; j < 4; j++) {
		for (i = 0; i < 4; i++) {
			node = ij + i;
			w = wx[i] * wy[j];
			retval += grd[node] * w;
			wsum += w;
		}
		ij += hdr.nx;
	}
	if (wsum > 0.0)
		retval /= wsum;
	else
		retval = 0;

	return retval;
}

/* ---------------------------------------------------------------------------------------- */
unsigned int gmt_bcr_prep (struct grd_header hdr, double xx, double yy, double wx[], double wy[]) {
	int col, row;
	unsigned int ij;
	double x, y, wp, wq, w, xi, yj;

	/* Compute the normalized real indices (x,y) of the point (xx,yy) within the grid.
	   Note that the y axis points down from the upper left corner of the grid. */

	x = (xx - hdr.x_min) / hdr.x_inc;
	y = (yy - hdr.y_min) / hdr.y_inc;

	/* Find the indices (i,j) of the node to the upper left of that.
   	   Because of padding, i and j can be on the edge. */
	xi  = floor (x);
	yj  = floor (y);
	col = irint (xi);
	row = irint (yj);

	/* Determine the offset of (x,y) with respect to (i,j). */
	x -= xi;
	y -= yj;

	/* For 4x4 interpolants, move over one more cell to the upper left corner */
	col--; row--;

	/* Save the location of the upper left corner point of the convolution kernel */
	ij = ij_grd(col, row, hdr);

	/* Build weights */

	/* These weights are based on the cubic convolution kernel, see for example
	   http://undergraduate.csse.uwa.edu.au/units/CITS4241/Handouts/Lecture04.html
		  These weights include a free parameter (a), which is set to -0.5 in this case.

	   In the absence of NaNs, the result of this is identical to the scheme introduced
	   by Walter Smith. The current implementation, however, is much less complex, faster,
	   allows NaNs to be skipped, and much more similar to the bilinear case.

	   Remko Scharroo, 10 Sep 2007.
	*/
	w = 1.0 - x;
	wp = w * x;
	wq = -0.5 * wp;
	wx[0] = wq * w;
	wx[3] = wq * x;
	wx[1] = 3 * wx[3] + w + wp;
	wx[2] = 3 * wx[0] + x + wp;

	w = 1.0 - y;
	wp = w * y;
	wq = -0.5 * wp;
	wy[0] = wq * w;
	wy[3] = wq * y;
	wy[1] = 3 * wy[3] + w + wp;
	wy[2] = 3 * wy[0] + y + wp;

	return (ij);
}

/* ---------------------------------------------------------------------------------------- */
void update_max(struct nestContainer *nest) {
	/* Update the max level at this iteration. The issue is that computing the maximum of nested
	   grids cannot be donne in the main loop because doughter grids are run much more time steps.
	   The difference may be substancial, specially because aliasing may be bloody striking.  */

	unsigned int ij;
	int writeLevel = nest->writeLevel;
	for (ij = 0; ij < nest->hdr[writeLevel].nm; ij++) {
		nest->work[ij] = (float)nest->etad[writeLevel][ij];
		if (nest->bat[writeLevel][ij] < 0) {
			if ((nest->work[ij] = (float)(nest->etaa[writeLevel][ij] + nest->bat[writeLevel][ij])) < 0)
				nest->work[ij] = 0;
		}
		if (nest->wmax[ij] < nest->work[ij])
			nest->wmax[ij] = nest->work[ij];
	}
}

/* ---------------------------------------------------------------------------------------- */
void update_max_velocity(struct nestContainer *nest) {
	/* Update the max velocity at this iteration. */
	unsigned int ij;
	int writeLevel = nest->writeLevel;
	float v;
	double vx, vy;

	for (ij = 0; ij < nest->hdr[writeLevel].nm; ij++) {
		vx = vy = 0;
		if (nest->htotal_d[writeLevel][ij] > EPS2)
			vx = nest->vex[writeLevel][ij];

		if (nest->htotal_d[writeLevel][ij] > EPS2)
			vy = nest->vey[writeLevel][ij];

		v = (float)(vx * vx + vy * vy);

		if (nest->htotal_d[writeLevel][ij] < 0.1 && v > 400)	/* Clip above this combination (400 = V_LIMIT * V_LIMIT) */
			v = 0;

		if (nest->vmax[ij] < v) nest->vmax[ij] = v;
	}
}
