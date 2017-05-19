/******************************************************************************
 *               libpluto -  A library version of Pluto                       *
 ******************************************************************************
 *                                                                            *
 * Copyright (C) 2012 Uday Bondhugula                                         *
 *                                                                            *
 * This library is free software; you can redistribute it and/or              *
 * modify it under the terms of the GNU Lesser General Public                 *
 * License version 2.1 as published by the Free Software Foundation.          *
 *                                                                            *
 * This library is distributed in the hope that it will be useful,            *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of             *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU          *
 * Lesser General Public License for more details.                            *
 *                                                                            *
 * A copy of the GNU Lesser General Public Licence can be found in the file
 * `LICENSE.LGPL2' in the top-level directory of this distribution.
 *
 */

#include <assert.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/time.h>

#include "pluto.h"
#include "constraints.h"
#include "candl/candl.h"
#include "program.h"
#include "pluto/libpluto.h"
#include "isl/map.h"
#include "isl/polynomial.h"
#include "barvinok/isl.h"
#include "candl/scop.h"

#define DEFAULT_L1_CACHE_SIZE 32768
#define DEFAULT_L1_CACHE_LINESIZE 64
#define DEFAULT_DATA_TYPE double

PlutoOptions *options;

/* Copied from petext.c */
struct pluto_access_meta_info {
    /* Pointer to an array of accesses */
    PlutoAccess ***accs;
    int index;
    int stmt_dim;
    int npar;
};


struct pluto_tile_footprint_meta_info {
    PlutoProg *prog;
    isl_union_map *sched;
    long *tile_footprint;
};

/* Copied from petext.c */
/* Extract a Pluto access function from isl_basic_map */
static int isl_basic_map_extract_access_func(__isl_take isl_basic_map *bmap, void *user)
{
    int i;

    isl_map *map;

    // isl_basic_map_dump(bmap);

    map = isl_map_from_basic_map(bmap);

    int dim = isl_map_dim(map, isl_dim_out);
    int ncols = isl_map_dim(map, isl_dim_in)
	+ isl_map_dim(map, isl_dim_param) + 1;

    PlutoMatrix *func = pluto_matrix_alloc(0, ncols);

    for (i=0; i<dim; i++) {
	PlutoMatrix *func_onedim = NULL;
	if (isl_map_dim_is_single_valued(map, i)) {
	    isl_pw_aff *pw_aff = isl_pw_aff_from_map_dim(map, i);
	    // isl_pw_aff_dump(pw_aff);
	    /* Best effort: Gets it from the last piece */
	    isl_pw_aff_foreach_piece(pw_aff, isl_aff_to_pluto_func, &func_onedim);
	    pluto_matrix_add(func, func_onedim);
	    pluto_matrix_free(func_onedim);
	    isl_pw_aff_free(pw_aff);
	}else{
	    pluto_matrix_add_row(func, 0);
	    pluto_matrix_zero_row(func, 0);
	}
    }
    struct pluto_access_meta_info *info = (struct pluto_access_meta_info *) user;

    (*info->accs)[info->index] = (PlutoAccess *) malloc(sizeof(PlutoAccess));
    PlutoAccess *acc = (*info->accs)[info->index];
    acc->name = strdup(isl_basic_map_get_tuple_name(bmap, isl_dim_out));
    acc->mat = func;

    info->index++;

    isl_map_free(map);

    return 0;
}

/* Extract Pluto access functions from isl_map */
int isl_map_extract_access_func(__isl_take isl_map *map, void *user)
{
    int r;

    /* Extract a PlutoAccess from every isl_basic_map */
    r = isl_map_foreach_basic_map(map, &isl_basic_map_extract_access_func, user);

    isl_map_free(map);

    return r;
}

static double rtclock()
{
    struct timezone Tzp;
    struct timeval Tp;
    int stat;
    stat = gettimeofday(&Tp, &Tzp);
    if (stat != 0) printf("Error return from gettimeofday: %d",stat);
    return(Tp.tv_sec + Tp.tv_usec*1.0e-6);
}

/* Temporary data structure used inside extra_stmt_domains
 *
 * stmts points to the array of Stmts being constructed
 * index is the index of the next stmt in the array
 */
struct pluto_extra_stmt_info {
    Stmt **stmts;
    int index;
};

struct pluto_extra_access_info {
    isl_union_map *access_fn;
    int id;
};

static int extract_basic_set(__isl_take isl_basic_set *bset, void *user)
{
    Stmt **stmts;
    Stmt *stmt;
    PlutoConstraints *bcst;
    struct pluto_extra_stmt_info *info;

    info = (struct pluto_extra_stmt_info *)user;

    stmts = info->stmts;
    stmt = stmts[info->index];

    bcst = isl_basic_set_to_pluto_constraints(bset);
    if (stmt->domain) {
        stmt->domain = pluto_constraints_unionize_simple(stmt->domain, bcst);
        pluto_constraints_free(bcst);
    }else{
        stmt->domain = bcst;
    }

    isl_basic_set_free(bset);
    return 0;
}

/* Used by libpluto interface */
static int extract_stmt(__isl_take isl_set *set, void *user)
{
    int r;
    Stmt **stmts;
    int id, i;

    stmts = (Stmt **) user;

    int dim = isl_set_dim(set, isl_dim_all);
    int npar = isl_set_dim(set, isl_dim_param);
    PlutoMatrix *trans = pluto_matrix_alloc(dim-npar, dim+1);
    pluto_matrix_set(trans, 0);
    trans->nrows = 0;

    /* A statement's domain (isl_set) should be named S_%d */
    const char *name = isl_set_get_tuple_name(set);
    assert(name);
    assert(strlen(name) >= 3);
    assert(name[0] == 'S');
    assert(name[1] == '_');
    assert(isdigit(name[2]));
    id = atoi(isl_set_get_tuple_name(set)+2);

    stmts[id] = pluto_stmt_alloc(dim-npar, NULL, trans);

    Stmt *stmt = stmts[id];
    stmt->type = ORIG;
    stmt->id = id;

    for (i=0; i<stmt->dim; i++) {
        char *iter = malloc(5);
        sprintf(iter, "i%d",  i);
        stmt->iterators[i] = iter;
    }

    struct pluto_extra_stmt_info info = {stmts, id};
    r = isl_set_foreach_basic_set(set, &extract_basic_set, &info);

    pluto_constraints_set_names_range(stmt->domain, stmt->iterators, 0, 0, stmt->dim);

    for (i=0; i<npar; i++) {
        char *param = malloc(5);
        sprintf(param, "p%d", i);
        stmt->domain->names[stmt->dim+i] = param;
    }

    pluto_matrix_free(trans);

    int j;
    for (j=0; j<stmt->dim; j++)  {
        stmt->is_orig_loop[j] = true;
    }

    isl_set_free(set);

    return r;
}

/* Used by libpluto interface */
static int extract_stmts(__isl_keep isl_union_set *domains, Stmt **stmts)
{
    isl_union_set_foreach_set(domains, &extract_stmt, stmts);

    return 0;
}

static int extract_access_fns(__isl_keep isl_union_map *reads,
	__isl_keep isl_union_map *writes,
	Stmt *stmt,
	int id)
{
    int j;
    isl_union_map_foreach_map(reads, &isl_map_count, &stmt->nreads);
    isl_union_map_foreach_map(writes, &isl_map_count, &stmt->nwrites);

    struct pluto_access_meta_info e_reads = {&stmt->reads, 0, stmt->dim, 0};
    struct pluto_access_meta_info e_writes = {&stmt->writes, 0, stmt->dim, 0};

    if (stmt->nreads >= 1) {
	stmt->reads = (PlutoAccess **) malloc(stmt->nreads*sizeof(PlutoAccess *));
    }
    if (stmt->nwrites >= 1) {
	stmt->writes = (PlutoAccess **) malloc(stmt->nwrites*sizeof(PlutoAccess *));
    }
    for (j=0; j<stmt->nreads; j++) {
	stmt->reads[j] = NULL;
    }
    for (j=0; j<stmt->nwrites; j++) {
	stmt->writes[j] = NULL;
    }

    isl_union_map_foreach_map(reads, &isl_map_extract_access_func, &e_reads);
    isl_union_map_foreach_map(writes, &isl_map_extract_access_func, &e_writes);

    //pluto_matrix_print(stdout, stmt->reads[0]->mat);
    return 0;
}

static int extract_stmt_access(__isl_take isl_map *map, void *user)
{
    struct pluto_extra_access_info *info = (struct pluto_extra_access_info *) user;
    isl_union_map *new_map = info->access_fn;
    int id = info->id;

    if (id == atoi(isl_map_get_tuple_name(map, isl_dim_in)+2)) {
	isl_union_map_add_map(new_map, isl_map_copy(map));
    }

    isl_map_free(map);
    return 0;
}

__isl_give isl_union_map *extract_stmt_accesses(__isl_keep isl_union_map *map, int id)
{
    isl_space *space = isl_union_map_get_space(map);
    isl_union_map *new_map = isl_union_map_empty(isl_space_copy(space));
    struct pluto_extra_access_info info = {new_map, id};
    isl_union_map_foreach_map(map, &extract_stmt_access, &info);

    isl_space_free(space);
    return new_map;
}

/*
 * Pluto tiling modifies the domain; move the information from the
 * domain to the schedules (inequalities will be added to the schedules)
 * Polly for LLVM expects domains to remain unchanged
 * (space/dimensionality-wise)
 */
PlutoConstraints *normalize_domain_schedule(Stmt *stmt, PlutoProg *prog)
{
    int del, r, c, j, snodes, nrows;
    PlutoConstraints *domain;

    PlutoConstraints *sched = pluto_stmt_get_schedule(stmt);

    domain = stmt->domain;
    snodes = stmt->dim - stmt->dim_orig;

    while (domain != NULL) {
        del = 0;
        nrows = domain->nrows;
        for (r=0; r<nrows; r++) {
            for (c=0; c<snodes; c++) {
                if (domain->val[r-del][c] != 0) break;
            }
            if (c < snodes) {
                PlutoConstraints *cut = pluto_constraints_select_row(domain, r-del);
                for (j=0; j<stmt->trans->nrows; j++) {
                    pluto_constraints_add_dim(cut, 0, NULL);
                }
                pluto_constraints_add(sched, cut);
                pluto_constraints_remove_row(domain, r-del);
                del++;
            }
        }
        domain = domain->next;
    }

    for (c=0; c<snodes; c++) {
        pluto_stmt_remove_dim(stmt, 0, prog);
    }
    return sched;
}

static int count_tile_footprint_access(__isl_take isl_map *map, void *user)
{
    struct pluto_tile_footprint_meta_info *ptfmi = (struct pluto_tile_footprint_meta_info *) user;
    isl_union_map *sched = ptfmi->sched;
    PlutoProg *prog = ptfmi->prog;
    isl_map *mem_to_stmt;
    int num, j, param_offset;
    isl_union_map *mem_to_sched, *mem_to_stmt2;
    isl_map *mem_to_sched_map;
    isl_space *space;
    isl_constraint *c;
    isl_pw_qpolynomial *card;
    isl_set *range;
    isl_ctx *ctx = isl_map_get_ctx(map);
    isl_val *max;
    long *tile_footprint = (long *)ptfmi->tile_footprint;
    int num_dim_after_last_tile_dim;


    sscanf(isl_map_get_tuple_name(map, isl_dim_in), "S_%d", &num);
    Stmt *stmt = prog->stmts[num];
    mem_to_stmt = isl_map_fixed_power_val(isl_map_copy(map),
                                          isl_val_negone(ctx));

    mem_to_stmt2 = isl_union_map_from_map(isl_map_copy(mem_to_stmt));
    mem_to_sched = isl_union_map_apply_range(isl_union_map_copy(mem_to_stmt2),
                                             isl_union_map_copy(sched));

    mem_to_sched_map = isl_map_from_union_map(isl_union_map_copy(mem_to_sched));
    //isl_map_dump(mem_to_sched_map);
    space = isl_map_get_space(mem_to_sched_map);
    if (isl_space_dim(space, isl_dim_in)==0 || stmt->last_tile_dim==-1) {
        isl_map_free(map);
        isl_space_free(space);
        isl_union_map_free(mem_to_sched);
        isl_map_free(mem_to_stmt);
        isl_union_map_free(mem_to_stmt2);
        isl_map_free(mem_to_sched_map);
        return isl_stat_ok;
    }

    num_dim_after_last_tile_dim = stmt->trans->nrows-stmt->last_tile_dim-1;
    mem_to_sched_map = isl_map_add_dims(mem_to_sched_map, isl_dim_out,
                                        isl_space_dim(space, isl_dim_in));

    isl_space_free(space);
    space = isl_map_get_space(mem_to_sched_map);

    for (j=0; j<isl_space_dim(space, isl_dim_in); j++) {
        int n = isl_space_dim(space, isl_dim_out)-isl_space_dim(space, isl_dim_in)+j;
        int div = DEFAULT_L1_CACHE_LINESIZE/sizeof(DEFAULT_DATA_TYPE);

        if (j == isl_space_dim(space, isl_dim_in)-1) {
          c = isl_constraint_alloc_inequality(isl_local_space_from_space(isl_space_copy(space)));
          c = isl_constraint_set_coefficient_si(c, isl_dim_out, n, div);
          c = isl_constraint_set_constant_si(c, div-1);
          c = isl_constraint_set_coefficient_si(c, isl_dim_in, j, -1);
          mem_to_sched_map = isl_map_add_constraint(mem_to_sched_map, c);

          c = isl_constraint_alloc_inequality(isl_local_space_from_space(isl_space_copy(space)));
          c = isl_constraint_set_coefficient_si(c, isl_dim_out, n, -div);
          c = isl_constraint_set_coefficient_si(c, isl_dim_in, j, 1);
        }
        else {
          c = isl_constraint_alloc_equality(isl_local_space_from_space(isl_space_copy(space)));
          c = isl_constraint_set_coefficient_si(c, isl_dim_out, n, 1);
          c = isl_constraint_set_coefficient_si(c, isl_dim_in, j, -1);
        }

        mem_to_sched_map = isl_map_add_constraint(mem_to_sched_map, isl_constraint_copy(c));
        isl_constraint_free(c);
    }

    isl_space_free(space);
    space = isl_map_get_space(mem_to_sched_map);

    mem_to_sched_map = isl_map_project_out(mem_to_sched_map,
                                           isl_dim_out,
                                           stmt->last_tile_dim+1,
                                           num_dim_after_last_tile_dim);

    mem_to_sched_map = isl_map_project_out(mem_to_sched_map,
                                           isl_dim_out,
                                           0,
                                           stmt->first_tile_dim);

    isl_space_free(space);
    space = isl_map_get_space(mem_to_sched_map);

    param_offset = isl_space_dim(space, isl_dim_param);
    mem_to_sched_map = isl_map_add_dims(mem_to_sched_map,
                     isl_dim_param,
                     isl_space_dim(space, isl_dim_out)-isl_space_dim(space, isl_dim_in));

    isl_space_free(space);
    space = isl_map_get_space(mem_to_sched_map);

    for (j=param_offset; j<isl_space_dim(space, isl_dim_param); j++) {
        c = isl_constraint_alloc_equality(isl_local_space_from_space(isl_space_copy(space)));
        c = isl_constraint_set_coefficient_si(c, isl_dim_param, j, 1);
        c = isl_constraint_set_coefficient_si(c, isl_dim_out, j-param_offset, -1);
        mem_to_sched_map = isl_map_add_constraint(mem_to_sched_map, isl_constraint_copy(c));
        isl_constraint_free(c);
    }

    mem_to_sched_map = isl_map_project_out(mem_to_sched_map, isl_dim_param, 0, param_offset);

    range = isl_map_range(isl_map_copy(mem_to_sched_map));
    card = isl_set_card(isl_set_copy(range));
    max = isl_pw_qpolynomial_max(card);

    *tile_footprint = *tile_footprint + isl_val_get_num_si(max) * DEFAULT_L1_CACHE_LINESIZE;

    isl_map_free(map);
    isl_map_free(mem_to_stmt);
    isl_map_free(mem_to_sched_map);
    isl_space_free(space);
    isl_union_map_free(mem_to_stmt2);
    isl_union_map_free(mem_to_sched);
    isl_set_free(range);
    isl_val_free(max);
    return isl_stat_ok;
}

int compute_tile_footprint(isl_union_set *domains,
                           isl_union_map *schedule,
                           isl_union_map *read,
                           isl_union_map *write,
                           PlutoProg *prog)
{
    isl_union_map *accesses, *sched;
    long tile_footprint=0;
    /* isl_union_map_dump(read); */
    /* isl_union_map_dump(schedule); */
    /* isl_union_set_dump(domains); */

    accesses = isl_union_map_union(isl_union_map_copy(read),
                                   isl_union_map_copy(write));
    sched = isl_union_map_intersect_domain(isl_union_map_copy(schedule),
                                           isl_union_set_copy(domains));
    struct pluto_tile_footprint_meta_info psmi = {prog, sched, &tile_footprint};
    isl_union_map_foreach_map(accesses, &count_tile_footprint_access, &psmi);
    printf("%li\n", tile_footprint);
    isl_union_map_free(accesses);
    isl_union_map_free(sched);
    return 0;
}


/* Construct isl_union_map for pluto schedules */
__isl_give isl_union_map *isl_union_map_for_pluto_schedule(PlutoProg *prog,
                                                           isl_ctx *ctx,
                                                           isl_space *space)
{
    int i, j;
    isl_union_map *schedules = isl_union_map_empty(isl_space_copy(space));
    for (i=0; i<prog->nstmts; i++) {
        isl_basic_map *bmap;
        isl_map *map;
        Stmt *stmt = prog->stmts[i];
        PlutoConstraints *sched = normalize_domain_schedule(stmt, prog);
        int n = sched->ncols - stmt->trans->nrows - stmt->domain->ncols;
        // pluto_constraints_print(stdout, sched);

        bmap = isl_basic_map_from_pluto_constraints(ctx, sched,
                sched->ncols - stmt->trans->nrows - prog->npar - 1,
                stmt->trans->nrows, prog->npar);
        bmap = isl_basic_map_project_out(bmap, isl_dim_in, 0, n);
        char name[20];
        snprintf(name, sizeof(name), "S_%d", i);
        map = isl_map_from_basic_map(bmap);

        /* Copy ids of the original parameter dimensions  */
        for (j=0; j<isl_space_dim(space, isl_dim_param); j++) {
            isl_id *id = isl_space_get_dim_id(space, isl_dim_param, j);
            map = isl_map_set_dim_id(map, isl_dim_param, j, id);
        }

        map = isl_map_set_tuple_name(map, isl_dim_in, name);
        schedules = isl_union_map_union(schedules, isl_union_map_from_map(map));

        pluto_constraints_free(sched);
    }
    return schedules;
}

/*
 * Output schedules are isl relations that have dims in the order
 * isl_dim_out, isl_dim_in, div, param, const
 */
__isl_give isl_union_map *pluto_schedule(isl_union_set *domains, 
	 isl_union_map *dependences,
     isl_union_map *read,
     isl_union_map *write,
     PlutoOptions *options_l)
{
    int i, nbands, n_ibands, retval;
    isl_ctx *ctx;
    isl_space *space;
    double t_t, t_all, t_start;
    isl_union_map *schedules;

    ctx = isl_union_set_get_ctx(domains);
    space = isl_union_set_get_space(domains);

    // isl_union_set_dump(domains);
    // isl_union_map_dump(dependences);

    PlutoProg *prog = pluto_prog_alloc();
    prog->options = options_l;

    /* global var */
    options = options_l;


    prog->nvar = -1;
    prog->nstmts = isl_union_set_n_set(domains);

    if (prog->nstmts >= 1) {
        prog->stmts = (Stmt **)malloc(prog->nstmts * sizeof(Stmt *));
    }else{
        prog->stmts = NULL;
    }

    for (i=0; i<prog->nstmts; i++) {
        prog->stmts[i] = NULL;
    }

    extract_stmts(domains, prog->stmts);

    for (i=0; i<prog->nstmts; i++) {
        prog->nvar = PLMAX(prog->nvar, prog->stmts[i]->dim);
    }

    for (i = 0; i<prog->nstmts; i++) {
	isl_union_map *reads = extract_stmt_accesses(read, i);
	isl_union_map *writes = extract_stmt_accesses(write, i);
	extract_access_fns(reads, writes, prog->stmts[i], i);
	isl_union_map_free(reads);
	isl_union_map_free(writes);
    }

    if (prog->nstmts >= 1) {
        Stmt *stmt = prog->stmts[0];
        prog->npar = stmt->domain->ncols - stmt->dim - 1;
        prog->params = (char **) malloc(sizeof(char *)*prog->npar);
    }else prog->npar = 0;

    for (i=0; i<prog->npar; i++) {
        char *param = malloc(5);
        sprintf(param, "p%d", i);
        prog->params[i] = param;
    }

    prog->ndeps = 0;
    isl_union_map_foreach_map(dependences, &isl_map_count, &prog->ndeps);

    prog->deps = (Dep **)malloc(prog->ndeps * sizeof(Dep *));
    for (i=0; i<prog->ndeps; i++) {
        prog->deps[i] = pluto_dep_alloc();
    }
    extract_deps(prog->deps, 0, prog->stmts,
            dependences, OSL_DEPENDENCE_RAW);

    IF_DEBUG(pluto_prog_print(stdout, prog););

    t_start = rtclock();
    retval = pluto_auto_transform(prog);
    t_t = rtclock() - t_start;

    if (retval) {
        /* Failure */
        pluto_prog_free(prog);
        isl_space_free(space);

        if (!options->silent) {
            printf("[libpluto] failure, returning NULL schedules\n");
        }

        return NULL;
    }

    pluto_compute_dep_directions(prog);
    pluto_compute_dep_satisfaction(prog);

    if (!options->silent) {
        fprintf(stdout, "[pluto] Affine transformations\n\n");
        /* Print out transformations */
        pluto_transformations_pretty_print(prog);
    }

    Band **bands, **ibands;
    bands = pluto_get_outermost_permutable_bands(prog, &nbands);
    ibands = pluto_get_innermost_permutable_bands(prog, &n_ibands);
    if (!options->silent) {
        printf("Outermost tilable bands: %d bands\n", nbands);
        pluto_bands_print(bands, nbands);
        printf("Innermost tilable bands: %d bands\n", n_ibands);
        pluto_bands_print(ibands, n_ibands);
    }

    if (options->tile) {
        pluto_tile(prog);
        schedules = isl_union_map_for_pluto_schedule(prog, ctx, space);
        compute_tile_footprint(domains, schedules, read, write, prog);
        isl_union_map_free(schedules);
    }else{
        if (options->intratileopt) {
            pluto_intra_tile_optimize(prog, 0);
        }
    }

    if ((options->parallel) && !options->tile && !options->identity)   {
        /* Obtain wavefront/pipelined parallelization by skewing if
         * necessary */
        int nbands;
        Band **bands;
        pluto_compute_dep_satisfaction(prog);
        bands = pluto_get_outermost_permutable_bands(prog, &nbands);
        bool retval = pluto_create_tile_schedule(prog, bands, nbands);
        pluto_bands_free(bands, nbands);

        /* If the user hasn't supplied --tile and there is only pipelined
         * parallelism, we will warn the user */
        if (retval)   {
            printf("[pluto] WARNING: pipelined parallelism exists and --tile is not used.\n");
            printf("[pluto] WARNING: use --tile for better parallelization \n");
            fprintf(stdout, "[pluto] After skewing:\n");
            pluto_transformations_pretty_print(prog);
            /* IF_DEBUG(pluto_print_hyperplane_properties(prog);); */
        }
    }

    if (options->parallel && !options->silent) {
        int nploops;
        Ploop **ploops = pluto_get_dom_parallel_loops(prog, &nploops);
        printf("[pluto_mark_parallel] %d parallel loops\n", nploops);
        pluto_loops_print(ploops, nploops);
        printf("\n");
        pluto_loops_free(ploops, nploops);
    }

    // pluto_stmts_print(stdout, prog->stmts, prog->nstmts);
    schedules = isl_union_map_for_pluto_schedule(prog, ctx, space);
    pluto_prog_free(prog);
    isl_space_free(space);

    t_all = rtclock() - t_start;

    if (options->time && !options->silent) {
        printf("[pluto] Auto-transformation time: %0.6lfs\n", t_t);
        printf("[pluto] Other/Misc time: %0.6lfs\n", t_all-t_t);
        printf("[pluto] Total time: %0.6lfs\n", t_all);
    }

    return schedules;
}

Remapping *pluto_get_remapping(isl_union_set *domains,
        isl_union_map *dependences, PlutoOptions *options_l) 
{

    int i, nbands, n_ibands, retval;
    isl_space *space;

    space = isl_union_set_get_space(domains);

    // isl_union_set_dump(domains);
    // isl_union_map_dump(dependences);

    PlutoProg *prog = pluto_prog_alloc();
    prog->options = options_l;

    /* global var */
    options = options_l;


    prog->nvar = -1;
    prog->nstmts = isl_union_set_n_set(domains);

    if (prog->nstmts >= 1) {
        prog->stmts = (Stmt **)malloc(prog->nstmts * sizeof(Stmt *));
    }else{
        prog->stmts = NULL;
    }

    for (i=0; i<prog->nstmts; i++) {
        prog->stmts[i] = NULL;
    }

    extract_stmts(domains, prog->stmts);

    for (i=0; i<prog->nstmts; i++) {
        prog->nvar = PLMAX(prog->nvar, prog->stmts[i]->dim);
    }

    if (prog->nstmts >= 1) {
        Stmt *stmt = prog->stmts[0];
        prog->npar = stmt->domain->ncols - stmt->dim - 1;
        prog->params = (char **) malloc(sizeof(char *)*prog->npar);
    }else prog->npar = 0;

    for (i=0; i<prog->npar; i++) {
        char *param = malloc(5);
        sprintf(param, "p%d", i);
        prog->params[i] = param;
    }

    prog->ndeps = 0;
    isl_union_map_foreach_map(dependences, &isl_map_count, &prog->ndeps);

    prog->deps = (Dep **)malloc(prog->ndeps * sizeof(Dep *));
    for (i=0; i<prog->ndeps; i++) {
        prog->deps[i] = pluto_dep_alloc();
    }
    extract_deps(prog->deps, 0, prog->stmts,
            dependences, OSL_DEPENDENCE_RAW);

    IF_DEBUG(pluto_prog_print(stdout, prog););

    retval = pluto_auto_transform(prog);

    if (retval) {
        /* Failure */
        pluto_prog_free(prog);
        isl_space_free(space);

        if (!options->silent) {
            printf("[libpluto] failure, returning NULL schedules\n");
        }

        return NULL;
    }

    pluto_compute_dep_directions(prog);
    pluto_compute_dep_satisfaction(prog);

    if (!options->silent) {
        fprintf(stdout, "[pluto] Affine transformations\n\n");
        /* Print out transformations */
        pluto_transformations_pretty_print(prog);
    }

    Band **bands, **ibands;
    bands = pluto_get_outermost_permutable_bands(prog, &nbands);
    ibands = pluto_get_innermost_permutable_bands(prog, &n_ibands);
    printf("Outermost tilable bands: %d bands\n", nbands);
    pluto_bands_print(bands, nbands);
    printf("Innermost tilable bands: %d bands\n", n_ibands);
    pluto_bands_print(ibands, n_ibands);

    if (options->tile) {
        pluto_tile(prog);
    }else{
        if (options->intratileopt) {
            pluto_intra_tile_optimize(prog, 0);
        }
    }

    Remapping *remapping = (Remapping *)malloc(sizeof(Remapping));
    remapping->nstmts = prog->nstmts;
    remapping->stmt_inv_matrices =
        (PlutoMatrix **)malloc(sizeof(PlutoMatrix *) * prog->nstmts);
    remapping->stmt_divs = (int **)malloc(sizeof(int *) * prog->nstmts);


    for(i = 0; i < prog->nstmts; i++) {
         remapping->stmt_inv_matrices[i] = pluto_stmt_get_remapping(prog->stmts[i],
                &remapping->stmt_divs[i]);
    }
    return remapping;
}



/*
 * Performs pluto-scheduling on an osl_scop.
 * If dependences are not found in Extensions, it'll recalculate them.
 * The osl_scop's statements' domains and scattering are replaced
 * by new ones.
 */
int pluto_schedule_osl(osl_scop_p scop, 
        PlutoOptions *options_l)
{
  int i=0;

  if (!scop || !scop->statement) {
    fprintf(stderr, "Empty Scop passed\n");
    return EXIT_FAILURE;
  }


  options = options_l;

  /* Convert clan scop to Pluto program */
  PlutoProg *prog = scop_to_pluto_prog(scop, options);

  int dim_sum=0;
  for (i=0; i<prog->nstmts; i++) {
      dim_sum += prog->stmts[i]->dim;
  }

  /* Make options consistent */
  if (options->multipar == 1 && options->parallel == 0)    {
      fprintf(stdout, "Warning: multipar needs parallel to be on; turning on parallel\n");
      options->parallel = 1;
  }

  /* Disable pre-vectorization if tile is not on */
  if (options->tile == 0 && options->prevector == 1) {
      /* If code will not be tiled, pre-vectorization does not make
       * sense */
      if (!options->silent)   {
          fprintf(stdout, "[Pluto] Warning: pre-vectorization does not fit (--tile is off)\n");
      }
      options->prevector = 0;
  }

  if (!options->silent)   {
      fprintf(stdout, "[Pluto] Number of statements: %d\n", prog->nstmts);
      fprintf(stdout, "[Pluto] Total number of loops: %d\n", dim_sum);
      fprintf(stdout, "[Pluto] Number of deps: %d\n", prog->ndeps);
      fprintf(stdout, "[Pluto] Maximum domain dimensionality: %d\n", prog->nvar);
      fprintf(stdout, "[Pluto] Number of parameters: %d\n", prog->npar);
  }


  /* Auto transformation */
  if (!options->identity) {
      pluto_auto_transform(prog);
  }

  pluto_compute_dep_directions(prog);
  pluto_compute_dep_satisfaction(prog);

  if (!options->silent)   {
      fprintf(stdout, "[Pluto] Affine transformations [<iter coeff's> <const>]\n\n");
      /* Print out transformations */
      pluto_transformations_pretty_print(prog);
  }

  if (options->tile)   {
      pluto_tile(prog);
  }else{
      if (options->intratileopt) {
          pluto_intra_tile_optimize(prog, 0); 
      }
  }

  if (options->parallel && !options->tile && !options->identity)   {
      /* Obtain wavefront/pipelined parallelization by skewing if
       * necessary */
      int nbands;
      Band **bands;
      pluto_compute_dep_satisfaction(prog);
      bands = pluto_get_outermost_permutable_bands(prog, &nbands);
      bool retval = pluto_create_tile_schedule(prog, bands, nbands);
      pluto_bands_free(bands, nbands);

      /* If the user hasn't supplied --tile and there is only pipelined
       * parallelism, we will warn the user */
      if (retval)   {
          printf("[pluto] WARNING: pipelined parallelism exists and --tile is not used.\n");
          printf("[pluto] WARNING: use --tile for better parallelization \n");
          fprintf(stdout, "[pluto] After skewing:\n");
          pluto_transformations_pretty_print(prog);
          /* IF_DEBUG(pluto_print_hyperplane_properties(prog);); */
}
  }

  if (options->unroll || options->polyunroll)    {
      /* Will generate a .unroll file */
      /* plann/plorc needs a .params */
      FILE *paramsFP = fopen(".params", "w");
      if (paramsFP)   {
          int i;
          for (i=0; i<prog->npar; i++)  {
              fprintf(paramsFP, "%s\n", prog->params[i]);
          }
          fclose(paramsFP);
      }
      pluto_detect_mark_unrollable_loops(prog);
  }

  if (options->polyunroll)    {
      /* Experimental */
      for (i=0; i<prog->num_hyperplanes; i++)   {
          if (prog->hProps[i].unroll)  {
              unroll_phis(prog, i, options->ufactor);
          }
      }
  }

  /* NO MORE TRANSFORMATIONS BEYOND THIS POINT */
  

  /* Replace the osl_scop's original domains and scatterings
   * by ones newly created by pluto
   */
  pluto_populate_scop (scop, prog, options);

  pluto_prog_free(prog);

  return EXIT_SUCCESS;
}

/* Pluto_schedule method to get schedule, parallel loops and remapping
*  all in one function
*/
__isl_give isl_union_map *pluto_parallel_schedule_with_remapping(isl_union_set *domains,
        isl_union_map *dependences,
        Ploop*** ploops,
        int* nploops,
        Remapping** remap,
        PlutoOptions *options_l)
{
    int i, j, nbands, n_ibands, retval;
    isl_ctx *ctx;
    isl_space *space;
    double t_t, t_all, t_start;

    ctx = isl_union_set_get_ctx(domains);
    space = isl_union_set_get_space(domains);

    PlutoProg *prog = pluto_prog_alloc();
    prog->options = options_l;

    /* global var */
    options = options_l;


    prog->nvar = -1;
    prog->nstmts = isl_union_set_n_set(domains);

    if (prog->nstmts >= 1) {
        prog->stmts = (Stmt **)malloc(prog->nstmts * sizeof(Stmt *));
    }else{
        prog->stmts = NULL;
    }

    for (i=0; i<prog->nstmts; i++) {
        prog->stmts[i] = NULL;
    }

    extract_stmts(domains, prog->stmts);

    for (i=0; i<prog->nstmts; i++) {
        prog->nvar = PLMAX(prog->nvar, prog->stmts[i]->dim);
    }

    if (prog->nstmts >= 1) {
        Stmt *stmt = prog->stmts[0];
        prog->npar = stmt->domain->ncols - stmt->dim - 1;
        prog->params = (char **) malloc(sizeof(char *)*prog->npar);
    }else prog->npar = 0;

    for (i=0; i<prog->npar; i++) {
        char *param = malloc(5);
        sprintf(param, "p%d", i);
        prog->params[i] = param;
    }

    prog->ndeps = 0;
    isl_union_map_foreach_map(dependences, &isl_map_count, &prog->ndeps);

    prog->deps = (Dep **)malloc(prog->ndeps * sizeof(Dep *));
    for (i=0; i<prog->ndeps; i++) {
        prog->deps[i] = pluto_dep_alloc();
    }
    extract_deps(prog->deps, 0, prog->stmts,
            dependences, OSL_DEPENDENCE_RAW);

    IF_DEBUG(pluto_prog_print(stdout, prog););

    t_start = rtclock();
    retval = pluto_auto_transform(prog);
    t_t = rtclock() - t_start;

    if (retval) {
        /* Failure */
        pluto_prog_free(prog);
        isl_space_free(space);

        if (!options->silent) {
            printf("[libpluto] failure, returning NULL schedules\n");
        }

        return NULL;
    }

    pluto_compute_dep_directions(prog);
    pluto_compute_dep_satisfaction(prog);

    if (!options->silent) {
        fprintf(stdout, "[pluto] Affine transformations\n\n");
        /* Print out transformations */
        pluto_transformations_pretty_print(prog);
    }

    Band **bands, **ibands;
    bands = pluto_get_outermost_permutable_bands(prog, &nbands);
    ibands = pluto_get_innermost_permutable_bands(prog, &n_ibands);
    printf("Outermost tilable bands: %d bands\n", nbands);
    pluto_bands_print(bands, nbands);
    printf("Innermost tilable bands: %d bands\n", n_ibands);
    pluto_bands_print(ibands, n_ibands);

    if (options->tile) {
        pluto_tile(prog);
    }else{
        if (options->intratileopt) {
            pluto_intra_tile_optimize(prog, 0);
        }
    }

    if ((options->parallel) && !options->tile && !options->identity)   {
        /* Obtain wavefront/pipelined parallelization by skewing if
         * necessary */
        int nbands;
        Band **bands;
        pluto_compute_dep_satisfaction(prog);
        bands = pluto_get_outermost_permutable_bands(prog, &nbands);
        bool retval = pluto_create_tile_schedule(prog, bands, nbands);
        pluto_bands_free(bands, nbands);

        /* If the user hasn't supplied --tile and there is only pipelined
         * parallelism, we will warn the user */
        if (retval)   {
            printf("[pluto] WARNING: pipelined parallelism exists and --tile is not used.\n");
            printf("[pluto] WARNING: use --tile for better parallelization \n");
            fprintf(stdout, "[pluto] After skewing:\n");
            pluto_transformations_pretty_print(prog);
            /* IF_DEBUG(pluto_print_hyperplane_properties(prog);); */
        }
    }

    if (options->parallel) {
        *ploops = pluto_get_parallel_loops(prog, nploops);
        if (!options->silent) {
            printf("[pluto_mark_parallel] %d parallel loops\n", *nploops);
            pluto_loops_print(*ploops, *nploops);
            printf("\n");
        }
    }else{
        *nploops = 0;
    }

    // Constructing remapping Matrix
    Remapping *remapping = (Remapping *)malloc(sizeof(Remapping));
    remapping->nstmts = prog->nstmts;
    remapping->stmt_inv_matrices =
        (PlutoMatrix **)malloc(sizeof(PlutoMatrix *) * prog->nstmts);
    remapping->stmt_divs = (int **)malloc(sizeof(int *) * prog->nstmts);

    *remap = remapping;

    for(i = 0; i < prog->nstmts; i++) {
         remapping->stmt_inv_matrices[i] = pluto_stmt_get_remapping(prog->stmts[i],
                &remapping->stmt_divs[i]);
         if (!options->silent) {
             printf("[libpluto] Statement %d Id- %d\n", i, prog->stmts[i]->id);
             pluto_matrix_print(stdout, remapping->stmt_inv_matrices[i]);
         }
    }

    /* Construct isl_union_map for pluto schedules */
    isl_union_map *schedules = isl_union_map_empty(isl_space_copy(space));
    for (i=0; i<prog->nstmts; i++) {
        isl_basic_map *bmap;
        isl_map *map;
        Stmt *stmt = prog->stmts[i];
        PlutoConstraints *sched = normalize_domain_schedule(stmt, prog);
        // pluto_constraints_print(stdout, sched);

        bmap = isl_basic_map_from_pluto_constraints(ctx, sched,
                sched->ncols - stmt->trans->nrows - prog->npar - 1,
                stmt->trans->nrows, prog->npar);
        bmap = isl_basic_map_project_out(bmap, isl_dim_in, 0, sched->ncols - stmt->trans->nrows - stmt->domain->ncols);
        char name[20];
        snprintf(name, sizeof(name), "S_%d", i);
        map = isl_map_from_basic_map(bmap);

        /* Copy ids of the original parameter dimensions  */
        for (j=0; j<isl_space_dim(space, isl_dim_param); j++) {
            isl_id *id = isl_space_get_dim_id(space, isl_dim_param, j);
            map = isl_map_set_dim_id(map, isl_dim_param, j, id);
        }

        map = isl_map_set_tuple_name(map, isl_dim_in, name);
        schedules = isl_union_map_union(schedules, isl_union_map_from_map(map));

        pluto_constraints_free(sched);
    }

    pluto_prog_free(prog);
    isl_space_free(space);

    t_all = rtclock() - t_start;

    if (options->time && !options->silent) {
        printf("[pluto] Auto-transformation time: %0.6lfs\n", t_t);
        printf("[pluto] Other/Misc time: %0.6lfs\n", t_all-t_t);
        printf("[pluto] Total time: %0.6lfs\n", t_all);
    }

    return schedules;
}

/* pluto_schedule_str is a wrapper method around
 * pluto_parallel_schedule_with_remapping().
 * This method accepts domain, dependence and PlutoOptions as string
 * and returns a transformed schedule, remapping and parallel loops.
 */
void pluto_schedule_str(const char *domains_str,
        const char *dependences_str,
        char** schedules_str_buffer_ptr,
        char** p_loops,
        Remapping **remapping_ptr,
        PlutoOptions *options) 
{
    isl_ctx *ctx = isl_ctx_alloc();
    Ploop** ploop;
    int nploop = 0,i;
    Remapping* remapping;

    isl_union_set *domains = isl_union_set_read_from_str(ctx, domains_str);
    isl_union_map *dependences = isl_union_map_read_from_str(ctx, 
            dependences_str);

    assert(remapping_ptr != NULL);
    isl_union_map *schedule = pluto_parallel_schedule_with_remapping(domains,
            dependences, &ploop, &nploop, &remapping, options);

    if (options->parallel) {
       if (!options->silent) {
           pluto_loops_print(ploop, nploop);
       }

       // NOTE: assuming max 4 digits
       // number of parallel loops
       char * str = (char *)(malloc(sizeof(char) * 5));
       sprintf(str, "%d", nploop);

       // NOTE: assuming max 4 digits per integer, and 1 char for comma
       // 1 place for 'nploop' int itself, and nploop places for the rest
       p_loops[0] = (char *) malloc(sizeof(char) * 6 * (nploop+1));
       strcpy(p_loops[0], str);

       for (i = 1; i < nploop + 1; i++) {
           // the result is a csv list
           strcat(p_loops[0], ",");
           // add the i'th parallel loop dim
           sprintf(str, "%d", ploop[i-1]->depth+1);
           strcat(p_loops[0], str);
       }
    }

    *remapping_ptr = remapping;

    isl_printer *printer = isl_printer_to_str(ctx);
    isl_printer_print_union_map(printer, schedule);
    
    *schedules_str_buffer_ptr = isl_printer_get_str(printer);
    assert(*schedules_str_buffer_ptr != NULL && "isl printer providing empty"
                                               " string");
   
    isl_printer_free(printer);
    isl_union_set_free(domains);
    isl_union_map_free(dependences);
    isl_union_map_free(schedule);

    isl_ctx_free(ctx);

}

void pluto_get_remapping_str(const char *domains_str,
        const char *dependences_str,
        Remapping **remapping_ptr,
        PlutoOptions *options) 
{
    isl_ctx *ctx = isl_ctx_alloc();
    isl_union_set *domains = isl_union_set_read_from_str(ctx, domains_str);
    isl_union_map *dependences = isl_union_map_read_from_str(ctx,
            dependences_str);

    assert(remapping_ptr != NULL);
    Remapping *remapping = pluto_get_remapping(domains, dependences, options);
    *remapping_ptr = remapping;

};


void pluto_remapping_free(Remapping *remapping) 
{
    assert(remapping != NULL);
    free(remapping);
};

void pluto_schedules_strbuf_free(char *schedules_str_buffer) 
{
  free(schedules_str_buffer);
}
