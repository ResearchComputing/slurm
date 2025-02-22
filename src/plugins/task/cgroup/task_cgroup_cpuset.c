/***************************************************************************** \
 *  task_cgroup_cpuset.c - cpuset cgroup subsystem for task/cgroup
 *****************************************************************************
 *  Copyright (C) 2009 CEA/DAM/DIF
 *  Written by Matthieu Hautreux <matthieu.hautreux@cea.fr>
 *  Portions copyright (C) 2012,2015 Bull/Atos
 *  Written by Martin Perry <martin.perry@atos.net>
 *
 *  This file is part of Slurm, a resource management program.
 *  For details, see <https://slurm.schedmd.com/>.
 *  Please also read the included file: DISCLAIMER.
 *
 *  Slurm is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  In addition, as a special exception, the copyright holders give permission
 *  to link the code of portions of this program with the OpenSSL library under
 *  certain conditions as described in each individual source file, and
 *  distribute linked combinations including the two. You must obey the GNU
 *  General Public License in all respects for all of the code used other than
 *  OpenSSL. If you modify file(s) with this exception, you may extend this
 *  exception to your version of the file(s), but you are not obligated to do
 *  so. If you do not wish to do so, delete this exception statement from your
 *  version.  If you delete this exception statement from all source files in
 *  the program, then also delete it here.
 *
 *  Slurm is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with Slurm; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
\*****************************************************************************/

#if !(defined(__FreeBSD__) || defined(__NetBSD__))
#include "config.h"

#define _GNU_SOURCE
#include <ctype.h>
#include <limits.h>
#include <sched.h>
#include <sys/types.h>

#include <slurm/slurm.h>
#include <slurm/slurm_errno.h>
#include "src/common/bitstring.h"
#include "src/common/cpu_frequency.h"
#include "src/common/proc_args.h"
#include "src/common/read_config.h"
#include "src/common/slurm_resource_info.h"
#include "src/common/xstring.h"
#include "src/common/cgroup.h"
#include "src/slurmd/common/xcpuinfo.h"
#include "src/slurmd/common/task_plugin.h"
#include "src/slurmd/slurmstepd/slurmstepd_job.h"
#include "src/slurmd/slurmd/slurmd.h"

#include "task_cgroup.h"

#ifdef HAVE_HWLOC
#include <hwloc.h>

#if !defined(__FreeBSD__)
#include <hwloc/glibc-sched.h>
#else
// For cpuset
#include <pthread_np.h>
#define cpu_set_t cpuset_t
#endif

# if HWLOC_API_VERSION <= 0x00010000
/*
 * After this version the cpuset structure and all it's functions
 * changed to bitmaps.  So to work with old hwloc's we just to the
 * opposite to avoid having to put a bunch of ifdef's in the code we
 * just do it here.
 */
typedef hwloc_cpuset_t hwloc_bitmap_t;
typedef hwloc_const_cpuset_t hwloc_const_bitmap_t;

static inline hwloc_bitmap_t hwloc_bitmap_alloc(void)
{
	return hwloc_cpuset_alloc();
}

static inline void hwloc_bitmap_free(hwloc_bitmap_t bitmap)
{
	hwloc_cpuset_free(bitmap);
}

static inline void hwloc_bitmap_or(
	hwloc_bitmap_t res, hwloc_bitmap_t bitmap1, hwloc_bitmap_t bitmap2)
{
	hwloc_cpuset_or(res, bitmap1, bitmap2);
}

static inline int hwloc_bitmap_asprintf(char **str, hwloc_bitmap_t bitmap)
{
	return hwloc_cpuset_asprintf(str, bitmap);
}

static inline int hwloc_bitmap_isequal(
	hwloc_const_bitmap_t bitmap1, hwloc_const_bitmap_t bitmap2)
{
	return hwloc_cpuset_isequal(bitmap1, bitmap2);
}

# endif

#if HWLOC_API_VERSION >= 0x00020000
static hwloc_bitmap_t global_allowed_cpuset;
#endif

static uint16_t bind_mode = CPU_BIND_NONE   | CPU_BIND_MASK   |
			    CPU_BIND_RANK   | CPU_BIND_MAP    |
			    CPU_BIND_LDMASK | CPU_BIND_LDRANK |
			    CPU_BIND_LDMAP;
static uint16_t bind_mode_ldom =
			    CPU_BIND_LDMASK | CPU_BIND_LDRANK |
			    CPU_BIND_LDMAP;

#endif

#ifdef HAVE_HWLOC

/*
 * Get sched cpuset for ldom
 *
 * in:  topology   = hwloc topology
 *      hwtype     = actual hardware type
 *      req_hwtype = requested hardware type
 *      ldom       = ldom#
 * out: mask       = pointer to sched cpuset
 */
static int _get_ldom_sched_cpuset(
		hwloc_topology_t topology,
		hwloc_obj_type_t hwtype, hwloc_obj_type_t req_hwtype,
		uint32_t ldom, cpu_set_t *mask);

/*
 * Get sched cpuset based on cpu_bind_type
 *
 * in:  topology   = hwloc topology
 *      hwtype     = actual hardware type
 *      req_hwtype = requested hardware type
 *      job        = pointer to job/step record
 * out: mask       = pointer to sched cpuset
 */
static int _get_sched_cpuset(
		hwloc_topology_t topology,
		hwloc_obj_type_t hwtype, hwloc_obj_type_t req_hwtype,
		cpu_set_t *mask, stepd_step_rec_t *job);

/*
 * Add hwloc cpuset for a hwloc object to the total cpuset for a task, using
 * the appropriate ancestor object cpuset if necessary
 *
 * in:  obj    = object to add
 * out: cpuset = hwloc cpuset for task
 */
static void _add_hwloc_cpuset(
		hwloc_obj_type_t hwtype, hwloc_obj_type_t req_hwtype,
		hwloc_obj_t obj, uint32_t taskid,  int bind_verbose,
		hwloc_bitmap_t cpuset);

/*
 * Distribute cpus to task using cyclic distribution across sockets
 *
 * in:  topology     = hwloc topology
 *      hwtype       = actual hardware type
 *      req_hwtype   = requested hardware type
 *      job          = pointer to job/step record
 *      bind_verbose = verbose option
 * out: cpuset       = hwloc cpuset for task
 */
static int _task_cgroup_cpuset_dist_cyclic(
	hwloc_topology_t topology, hwloc_obj_type_t hwtype,
	hwloc_obj_type_t req_hwtype, stepd_step_rec_t *job, int bind_verbose,
	hwloc_bitmap_t cpuset);

/*
 * Distribute cpus to task using block distribution
 *
 * in:  topology     = hwloc topology
 *      hwtype       = actual hardware type
 *      req_hwtype   = requested hardware type
 *      job          = pointer to job/step record
 *      bind_verbose = verbose option
 * out: cpuset       = hwloc cpuset for task
 */
static int _task_cgroup_cpuset_dist_block(
	hwloc_topology_t topology, hwloc_obj_type_t hwtype,
	hwloc_obj_type_t req_hwtype, uint32_t nobj,
	stepd_step_rec_t *job, int bind_verbose, hwloc_bitmap_t cpuset);

static int _get_ldom_sched_cpuset(hwloc_topology_t topology,
		hwloc_obj_type_t hwtype, hwloc_obj_type_t req_hwtype,
		uint32_t ldom, cpu_set_t *mask)
{
	hwloc_obj_t obj;
	hwloc_bitmap_t cpuset;
	int hwdepth;

	cpuset = hwloc_bitmap_alloc();
	hwdepth = hwloc_get_type_depth(topology, hwtype);
	obj = hwloc_get_obj_by_depth(topology, hwdepth, ldom);
	_add_hwloc_cpuset(hwtype, req_hwtype, obj, 0, 0, cpuset);
	hwloc_cpuset_to_glibc_sched_affinity(topology, cpuset,
						 mask, sizeof(cpu_set_t));
	hwloc_bitmap_free(cpuset);
	return true;
}

static int _get_sched_cpuset(hwloc_topology_t topology,
		hwloc_obj_type_t hwtype, hwloc_obj_type_t req_hwtype,
		cpu_set_t *mask, stepd_step_rec_t *job)
{
	int nummasks, maskid, i, threads;
	char *curstr, *selstr;
	char mstr[1 + CPU_SETSIZE / 4];
	uint32_t local_id = job->envtp->localid;
	char buftype[1024];

	/* For CPU_BIND_RANK, CPU_BIND_MASK and CPU_BIND_MAP, generate sched
	 * cpuset directly from cpu numbers.
	 * For CPU_BIND_LDRANK, CPU_BIND_LDMASK and CPU_BIND_LDMAP, generate
	 * sched cpuset from hwloc topology.
	 */
	slurm_sprint_cpu_bind_type(buftype, job->cpu_bind_type);
	debug3("(%s[%d]) %s", buftype,
			job->cpu_bind_type, job->cpu_bind);
	CPU_ZERO(mask);

	if (job->cpu_bind_type & CPU_BIND_NONE) {
		return true;
	}

	if (job->cpu_bind_type & CPU_BIND_RANK) {
		threads = MAX(conf->threads, 1);
		CPU_SET(job->envtp->localid % (job->cpus*threads), mask);
		return true;
	}

	if (job->cpu_bind_type & CPU_BIND_LDRANK) {
		return _get_ldom_sched_cpuset(topology, hwtype, req_hwtype,
				local_id, mask);
	}

	if (!job->cpu_bind)
		return false;

	nummasks = 1;
	selstr = NULL;

	/* get number of strings present in cpu_bind */
	curstr = job->cpu_bind;
	while (*curstr) {
		if (nummasks == local_id+1) {
			selstr = curstr;
			break;
		}
		if (*curstr == ',')
			nummasks++;
		curstr++;
	}

	/* if we didn't already find the mask... */
	if (!selstr) {
		/* ...select mask string by wrapping task ID into list */
		maskid = local_id % nummasks;
		i = maskid;
		curstr = job->cpu_bind;
		while (*curstr && i) {
			if (*curstr == ',')
			    	i--;
			curstr++;
		}
		if (!*curstr) {
			return false;
		}
		selstr = curstr;
	}

	/* extract the selected mask from the list */
	i = 0;
	curstr = mstr;
	while (*selstr && *selstr != ',' && i++ < (CPU_SETSIZE/4))
		*curstr++ = *selstr++;
	*curstr = '\0';

	if (job->cpu_bind_type & CPU_BIND_MASK) {
		/* convert mask string into cpu_set_t mask */
		if (task_str_to_cpuset(mask, mstr) < 0) {
			error("task_str_to_cpuset %s", mstr);
			return false;
		}
		return true;
	}

	if (job->cpu_bind_type & CPU_BIND_MAP) {
		unsigned int mycpu = 0;
		if (xstrncmp(mstr, "0x", 2) == 0) {
			mycpu = strtoul (&(mstr[2]), NULL, 16);
		} else {
			mycpu = strtoul (mstr, NULL, 10);
		}
		CPU_SET(mycpu, mask);
		return true;
	}

	if (job->cpu_bind_type & CPU_BIND_LDMASK) {
		int len = strlen(mstr);
		char *ptr = mstr + len - 1;
		uint32_t base = 0;

		curstr = mstr;
		/* skip 0x, it's all hex anyway */
		if (len > 1 && !memcmp(mstr, "0x", 2L))
			curstr += 2;
		while (ptr >= curstr) {
			char val = slurm_char_to_hex(*ptr);
			if (val == (char) -1)
				return false;
			if (val & 1)
				_get_ldom_sched_cpuset(topology, hwtype,
						req_hwtype, base, mask);
			if (val & 2)
				_get_ldom_sched_cpuset(topology, hwtype,
						req_hwtype, base + 1, mask);
			if (val & 4)
				_get_ldom_sched_cpuset(topology, hwtype,
						req_hwtype, base + 2, mask);
			if (val & 8)
				_get_ldom_sched_cpuset(topology, hwtype,
						req_hwtype, base + 3, mask);
			len--;
			ptr--;
			base += 4;
		}
		return true;
	}

	if (job->cpu_bind_type & CPU_BIND_LDMAP) {
		uint32_t myldom = 0;
		if (xstrncmp(mstr, "0x", 2) == 0) {
			myldom = strtoul (&(mstr[2]), NULL, 16);
		} else {
			myldom = strtoul (mstr, NULL, 10);
		}
		return _get_ldom_sched_cpuset(topology, hwtype, req_hwtype,
				myldom, mask);
	}

	return false;
}

static void _add_hwloc_cpuset(
	hwloc_obj_type_t hwtype, hwloc_obj_type_t req_hwtype,
	hwloc_obj_t obj, uint32_t taskid,  int bind_verbose,
	hwloc_bitmap_t cpuset)
{
#if HWLOC_API_VERSION >= 0x00020000
	hwloc_bitmap_t allowed_cpuset;
#endif
	struct hwloc_obj *pobj;

	/*
	 * if requested binding overlaps the granularity
	 * use the ancestor cpuset instead of the object one
	 */
	if (hwloc_compare_types(hwtype, req_hwtype) > 0) {

		/*
		 * Get the parent object of req_hwtype or the
		 * one just above if not found (meaning of >0
		 * (useful for ldoms binding with !NUMA nodes)
		 */
		pobj = obj->parent;
		while (pobj != NULL &&
			hwloc_compare_types(pobj->type, req_hwtype) > 0)
			pobj = pobj->parent;

		if (pobj != NULL) {
			if (bind_verbose)
				info("task[%u] higher level %s "
				     "found", taskid,
				     hwloc_obj_type_string(pobj->type));
#if HWLOC_API_VERSION >= 0x00020000
			allowed_cpuset = hwloc_bitmap_alloc();
			hwloc_bitmap_and(allowed_cpuset, global_allowed_cpuset,
					 pobj->cpuset);
			hwloc_bitmap_or(cpuset, cpuset, allowed_cpuset);
			hwloc_bitmap_free(allowed_cpuset);
#else
			hwloc_bitmap_or(cpuset, cpuset, pobj->allowed_cpuset);
#endif
		} else {
			/* should not be executed */
			if (bind_verbose)
				info("task[%u] no higher level "
				     "found", taskid);
#if HWLOC_API_VERSION >= 0x00020000
			allowed_cpuset = hwloc_bitmap_alloc();
			hwloc_bitmap_and(allowed_cpuset, global_allowed_cpuset,
					 obj->cpuset);
			hwloc_bitmap_or(cpuset, cpuset, allowed_cpuset);
			hwloc_bitmap_free(allowed_cpuset);
#else
			hwloc_bitmap_or(cpuset, cpuset, obj->allowed_cpuset);
#endif
		}

	} else {
#if HWLOC_API_VERSION >= 0x00020000
		allowed_cpuset = hwloc_bitmap_alloc();
		hwloc_bitmap_and(allowed_cpuset, global_allowed_cpuset,
				 obj->cpuset);
		hwloc_bitmap_or(cpuset, cpuset, allowed_cpuset);
		hwloc_bitmap_free(allowed_cpuset);
#else
		hwloc_bitmap_or(cpuset, cpuset, obj->allowed_cpuset);
#endif
	}
}

static int _task_cgroup_cpuset_dist_cyclic(
	hwloc_topology_t topology, hwloc_obj_type_t hwtype,
	hwloc_obj_type_t req_hwtype, stepd_step_rec_t *job, int bind_verbose,
	hwloc_bitmap_t cpuset)
{
#if HWLOC_API_VERSION >= 0x00020000
	hwloc_bitmap_t allowed_cpuset;
	bool allowed_cpuset_alloc = false;
#endif
	hwloc_obj_t obj;
	uint32_t  s_ix;		/* socket index */
	uint32_t *c_ixc;	/* core index by socket (current taskid) */
	uint32_t *c_ixn;	/* core index by socket (next taskid) */
	uint32_t *t_ix;		/* thread index by core by socket */
	uint16_t npus = 0, nboards = 0, nthreads = 0, ncores = 0, nsockets = 0;
	uint32_t taskid = job->envtp->localid;
	int spec_thread_cnt = 0;
	bitstr_t *spec_threads = NULL;
	uint32_t obj_idxs[3], cps, tpc, i, j, sock_loop, ntskip, npdist;
	bool core_cyclic, core_fcyclic, sock_fcyclic;
	bool hwloc_success = true;
	hwloc_obj_type_t socket_type = HWLOC_OBJ_SOCKET;
#if HWLOC_API_VERSION >= 0x00020000
	if (xstrcasestr(slurm_conf.slurmd_params, "l3cache_as_socket"))
		socket_type = HWLOC_OBJ_L3CACHE;
#endif

	/*
	 * We can't trust the slurmd_conf_t *conf here as we need actual
	 * hardware instead of whatever is possibly configured.  So we need to
	 * look it up again.
	 */
	if (xcpuinfo_hwloc_topo_get(
		    &npus, &nboards, &nsockets, &ncores, &nthreads,
		    NULL, NULL, NULL) != SLURM_SUCCESS) {
		/*
		 * Fall back to use allocated resources, but this may result
		 * in incorrect layout due to a uneven task distribution
		 * (e.g. 4 cores on socket 0 and 3 cores on socket 1)
		 */
		nsockets = (uint16_t) hwloc_get_nbobjs_by_type(topology,
							       socket_type);
		ncores = (uint16_t) hwloc_get_nbobjs_by_type(topology,
							HWLOC_OBJ_CORE);
		nthreads = (uint16_t) hwloc_get_nbobjs_by_type(topology,
							HWLOC_OBJ_PU);
		npus = (uint16_t) hwloc_get_nbobjs_by_type(topology,
							   HWLOC_OBJ_PU);
	} else {
		/* Translate cores-per-socket to total core count, etc. */
		nsockets *= nboards;
		ncores *= nsockets;
		nthreads *= ncores;
	}

	if ((nsockets == 0) || (ncores == 0))
		return SLURM_ERROR;
	cps = (ncores + nsockets - 1) / nsockets;
	tpc = (nthreads + ncores - 1) / ncores;

	sock_fcyclic = (job->task_dist & SLURM_DIST_SOCKMASK) ==
		SLURM_DIST_SOCKCFULL ? true : false;
	core_cyclic = (job->task_dist & SLURM_DIST_COREMASK) ==
		SLURM_DIST_CORECYCLIC ? true : false;
	core_fcyclic = (job->task_dist & SLURM_DIST_COREMASK) ==
		SLURM_DIST_CORECFULL ? true : false;

	if (bind_verbose) {
		info("task[%u] using %s distribution "
		     "(task_dist=0x%x)", taskid,
		     format_task_dist_states(job->task_dist), job->task_dist);
	}

	t_ix = xmalloc(ncores * sizeof(uint32_t));
	c_ixc = xmalloc(nsockets * sizeof(uint32_t));
	c_ixn = xmalloc(nsockets * sizeof(uint32_t));

	if (hwloc_compare_types(hwtype, HWLOC_OBJ_CORE) >= 0) {
		/* cores or threads granularity */
		ntskip = taskid;
		npdist = job->cpus_per_task;
	} else {
		/* sockets or ldoms granularity */
		ntskip = taskid;
		npdist = 1;
	}
	if ((job->job_core_spec != NO_VAL16) &&
	    (job->job_core_spec &  CORE_SPEC_THREAD)  &&
	    (job->job_core_spec != CORE_SPEC_THREAD)) {
		/* Skip specialized threads as needed */
		int i, t, c, s;
		int cores = (ncores + nsockets - 1) / nsockets;
		int threads = (npus + cores - 1) / cores;
		spec_thread_cnt = job->job_core_spec & (~CORE_SPEC_THREAD);
		spec_threads = bit_alloc(npus);
		for (t = threads - 1;
		     ((t >= 0) && (spec_thread_cnt > 0)); t--) {
			for (c = cores - 1;
			     ((c >= 0) && (spec_thread_cnt > 0)); c--) {
				for (s = nsockets - 1;
				     ((s >= 0) && (spec_thread_cnt > 0)); s--) {
					i = s * cores + c;
					i = (i * threads) + t;
					bit_set(spec_threads, i);
					spec_thread_cnt--;
				}
			}
		}
		if (hwtype == HWLOC_OBJ_PU) {
			for (i = 0; i <= ntskip && i < npus; i++) {
				if (bit_test(spec_threads, i))
					ntskip++;
			};
		}
	}

	/*
	 * skip objs for lower taskids, then add them to the
	 * current task cpuset. To prevent infinite loop, check
	 * that we do not loop more than npdist times around the available
	 * sockets, which is the worst scenario we should afford here.
	 */
	i = j = s_ix = sock_loop = 0;
	while (i < ntskip + 1 && (sock_loop/tpc) < npdist + 1) {
		/*
		 * fill one or multiple sockets using block mode, unless
		 * otherwise stated in the job->task_dist field
		 */
		while ((s_ix < nsockets) && (j < npdist)) {
			obj = hwloc_get_obj_below_by_type(
				topology, socket_type, s_ix,
				hwtype, c_ixc[s_ix]);
#if HWLOC_API_VERSION >= 0x00020000
			if (obj) {
				allowed_cpuset = hwloc_bitmap_alloc();
				allowed_cpuset_alloc = true;
				hwloc_bitmap_and(allowed_cpuset,
						 global_allowed_cpuset,
						 obj->cpuset);
			}
#endif
			if ((obj == NULL) && (s_ix == 0) && (c_ixc[s_ix] == 0))
				hwloc_success = false;	/* Complete failure */
			if ((obj != NULL) &&
#if HWLOC_API_VERSION >= 0x00020000
			    (hwloc_bitmap_first(allowed_cpuset) != -1)) {
#else
			    (hwloc_bitmap_first(obj->allowed_cpuset) != -1)) {
#endif
				if (hwloc_compare_types(hwtype, HWLOC_OBJ_PU)
									>= 0) {
					hwloc_obj_type_t obj_types[3] = {
						socket_type,
						HWLOC_OBJ_CORE,
						HWLOC_OBJ_PU
					};

					/* granularity is thread */
					obj_idxs[0]=s_ix;
					obj_idxs[1]=c_ixc[s_ix];
					obj_idxs[2]=t_ix[(s_ix*cps)+c_ixc[s_ix]];
					obj = hwloc_get_obj_below_array_by_type(
						topology, 3, obj_types, obj_idxs);
					if ((obj != NULL) &&
#if HWLOC_API_VERSION >= 0x00020000
					    (hwloc_bitmap_first(
					     allowed_cpuset) != -1)) {
#else
					    (hwloc_bitmap_first(
					     obj->allowed_cpuset) != -1)) {
#endif
						t_ix[(s_ix*cps)+c_ixc[s_ix]]++;
						j++;
						if (i == ntskip)
							_add_hwloc_cpuset(hwtype,
							req_hwtype, obj, taskid,
							bind_verbose, cpuset);
						if (j < npdist) {
							if (core_cyclic) {
								c_ixn[s_ix] =
								c_ixc[s_ix] + 1;
							} else if (core_fcyclic){
								c_ixc[s_ix]++;
								c_ixn[s_ix] =
								c_ixc[s_ix];
							}
							if (sock_fcyclic)
								s_ix++;
						}
					} else {
						c_ixc[s_ix]++;
						if (c_ixc[s_ix] == cps)
							s_ix++;
					}
				} else {
					/* granularity is core or larger */
					c_ixc[s_ix]++;
					j++;
					if (i == ntskip)
						_add_hwloc_cpuset(hwtype,
							req_hwtype, obj, taskid,
						  	bind_verbose, cpuset);
					if ((j < npdist) && (sock_fcyclic))
						s_ix++;
				}
			} else
				s_ix++;
#if HWLOC_API_VERSION >= 0x00020000
			if (allowed_cpuset_alloc) {
				hwloc_bitmap_free(allowed_cpuset);
				allowed_cpuset_alloc = false;
			}
#endif
		}
		/* if it succeeds, switch to the next task, starting
		 * with the next available socket, otherwise, loop back
		 * from the first socket trying to find available slots. */
		if (j == npdist) {
			i++;
			j = 0;
			s_ix++; // no validity check, handled by the while
			sock_loop = 0;
		} else {
			sock_loop++;
			s_ix = 0;
		}
	}
	xfree(t_ix);
	xfree(c_ixc);
	xfree(c_ixn);

	if (spec_threads) {
		for (i = 0; i < npus; i++) {
			if (bit_test(spec_threads, i)) {
				hwloc_bitmap_clr(cpuset, i);
			}
		}
		FREE_NULL_BITMAP(spec_threads);
	}

	/* should never happen in normal scenario */
	if ((sock_loop > npdist) && !hwloc_success) {
		/* hwloc_get_obj_below_by_type() fails if no CPU set
		 * configured, see hwloc documentation for details */
		error("hwloc_get_obj_below_by_type() failing, "
		      "task/affinity plugin may be required to address bug "
		      "fixed in HWLOC version 1.11.5");
		return SLURM_ERROR;
	} else if (sock_loop > npdist) {
		char buf[128] = "";
		hwloc_bitmap_snprintf(buf, sizeof(buf), cpuset);
		error("task[%u] infinite loop broken while trying "
		      "to provision compute elements using %s (bitmap:%s)",
		      taskid, format_task_dist_states(job->task_dist), buf);
		return SLURM_ERROR;
	} else
		return SLURM_SUCCESS;
}

static int _task_cgroup_cpuset_dist_block(
	hwloc_topology_t topology, hwloc_obj_type_t hwtype,
	hwloc_obj_type_t req_hwtype, uint32_t nobj,
	stepd_step_rec_t *job, int bind_verbose, hwloc_bitmap_t cpuset)
{
	hwloc_obj_t obj;
	uint32_t core_loop, ntskip, npdist;
	uint32_t i, j, pfirst, plast;
	uint32_t taskid = job->envtp->localid;
	int hwdepth;
	uint32_t npus, ncores, nsockets;
	int spec_thread_cnt = 0;
	bitstr_t *spec_threads = NULL;
	bool hwloc_success = true;

	uint32_t core_idx;
	bool core_fcyclic, core_block;

	hwloc_obj_type_t socket_type = HWLOC_OBJ_SOCKET;
#if HWLOC_API_VERSION >= 0x00020000
	if (xstrcasestr(slurm_conf.slurmd_params, "l3cache_as_socket"))
		socket_type = HWLOC_OBJ_L3CACHE;
#endif

	nsockets = (uint32_t) hwloc_get_nbobjs_by_type(topology,
						       socket_type);
	ncores = (uint32_t) hwloc_get_nbobjs_by_type(topology,
						     HWLOC_OBJ_CORE);
	npus = (uint32_t) hwloc_get_nbobjs_by_type(topology,
						   HWLOC_OBJ_PU);

	core_block = (job->task_dist & SLURM_DIST_COREMASK) ==
		SLURM_DIST_COREBLOCK ? true : false;
	core_fcyclic = (job->task_dist & SLURM_DIST_COREMASK) ==
		SLURM_DIST_CORECFULL ? true : false;

	if (bind_verbose) {
		info("task[%u] using block distribution, "
		     "task_dist 0x%x", taskid, job->task_dist);
	}

	if ((hwloc_compare_types(hwtype, HWLOC_OBJ_PU) == 0) && !core_block) {
		uint32_t *thread_idx = xmalloc(ncores * sizeof(uint32_t));
		ntskip = taskid;
		npdist = job->cpus_per_task;

		i = 0; j = 0;
		core_idx = 0;
		core_loop = 0;
		while (i < ntskip + 1 && core_loop < npdist + 1) {
			while ((core_idx < ncores) && (j < npdist)) {
				obj = hwloc_get_obj_below_by_type(
					topology, HWLOC_OBJ_CORE, core_idx,
					hwtype, thread_idx[core_idx]);
				if ((obj == NULL) && (core_idx == 0) &&
				    (thread_idx[core_idx] == 0))
					hwloc_success = false;
				if (obj != NULL) {
					thread_idx[core_idx]++;
					j++;
					if (i == ntskip)
						_add_hwloc_cpuset(hwtype,
							req_hwtype, obj, taskid,
							bind_verbose, cpuset);
					if ((j < npdist) && core_fcyclic)
						core_idx++;
				} else {
					core_idx++;
				}
			}
			if (j == npdist) {
				i++; j = 0;
				core_idx++; // no validity check, handled by the while
				core_loop = 0;
			} else {
				core_loop++;
				core_idx = 0;
			}
		}
		xfree(thread_idx);

		/* should never happen in normal scenario */
		if ((core_loop > npdist) && !hwloc_success) {
			/* hwloc_get_obj_below_by_type() fails if no CPU set
			 * configured, see hwloc documentation for details */
			error("hwloc_get_obj_below_by_type() "
			      "failing, task/affinity plugin may be required"
			      "to address bug fixed in HWLOC version 1.11.5");
			return SLURM_ERROR;
		} else if (core_loop > npdist) {
			char buf[128] = "";
			hwloc_bitmap_snprintf(buf, sizeof(buf), cpuset);
			error("task[%u] infinite loop broken while "
			      "trying to provision compute elements using %s (bitmap:%s)",
			      taskid, format_task_dist_states(job->task_dist),
			      buf);
			return SLURM_ERROR;
		} else
			return SLURM_SUCCESS;
	}

	if (hwloc_compare_types(hwtype, HWLOC_OBJ_CORE) >= 0) {
		/* cores or threads granularity */
		pfirst = taskid * job->cpus_per_task ;
		plast = pfirst + job->cpus_per_task - 1;
	} else {
		/* sockets or ldoms granularity */
		pfirst = taskid;
		plast = pfirst;
	}

	hwdepth = hwloc_get_type_depth(topology, hwtype);
	if ((job->job_core_spec != NO_VAL16) &&
	    (job->job_core_spec &  CORE_SPEC_THREAD)  &&
	    (job->job_core_spec != CORE_SPEC_THREAD)  &&
	    (nsockets != 0)) {
		/* Skip specialized threads as needed */
		int i, t, c, s;
		int cores = MAX(1, (ncores / nsockets));
		int threads = npus / cores;
		spec_thread_cnt = job->job_core_spec & (~CORE_SPEC_THREAD);
		spec_threads = bit_alloc(npus);
		for (t = threads - 1;
		     ((t >= 0) && (spec_thread_cnt > 0)); t--) {
			for (c = cores - 1;
			     ((c >= 0) && (spec_thread_cnt > 0)); c--) {
				for (s = nsockets - 1;
				     ((s >= 0) && (spec_thread_cnt > 0)); s--) {
					i = s * cores + c;
					i = (i * threads) + t;
					bit_set(spec_threads, i);
					spec_thread_cnt--;
				}
			}
		}
		if (hwtype == HWLOC_OBJ_PU) {
			for (i = 0; i <= pfirst && i < npus; i++) {
				if (bit_test(spec_threads, i))
					pfirst++;
			};
		}
	}

	for (i = pfirst; i <= plast && i < nobj ; i++) {
		obj = hwloc_get_obj_by_depth(topology, hwdepth, (int)i);
		_add_hwloc_cpuset(hwtype, req_hwtype, obj, taskid,
			    bind_verbose, cpuset);
	}

	if (spec_threads) {
		for (i = 0; i < npus; i++) {
			if (bit_test(spec_threads, i)) {
				hwloc_bitmap_clr(cpuset, i);
			}
		};
		FREE_NULL_BITMAP(spec_threads);
	}

	return SLURM_SUCCESS;
}


/* The job has specialized cores, synchronize user mask with available cores */
static void _validate_mask(uint32_t task_id, hwloc_obj_t obj, cpu_set_t *ts)
{
#if HWLOC_API_VERSION >= 0x00020000
	hwloc_bitmap_t allowed_cpuset;
#endif
	int i, j, overlaps = 0;
	bool superset = true;

#if HWLOC_API_VERSION >= 0x00020000
	allowed_cpuset = hwloc_bitmap_alloc();
	hwloc_bitmap_and(allowed_cpuset, global_allowed_cpuset, obj->cpuset);
#endif
	for (i = 0; i < CPU_SETSIZE; i++) {
		if (!CPU_ISSET(i, ts))
			continue;
#if HWLOC_API_VERSION >= 0x00020000
		j = hwloc_bitmap_isset(allowed_cpuset, i);
#else
		j = hwloc_bitmap_isset(obj->allowed_cpuset, i);
#endif
		if (j > 0) {
			overlaps++;
		} else if (j == 0) {
			CPU_CLR(i, ts);
			superset = false;
		}
	}

	if (overlaps == 0) {
		/*
		 * The task's cpu map is completely invalid.
		 * Give it all allowed CPUs
		 */
		for (i = 0; i < CPU_SETSIZE; i++) {
#if HWLOC_API_VERSION >= 0x00020000
			if (hwloc_bitmap_isset(allowed_cpuset, i) > 0)
#else
			if (hwloc_bitmap_isset(obj->allowed_cpuset, i) > 0)
#endif
				CPU_SET(i, ts);
		}
	}

	if (!superset) {
		info("Ignoring user CPU binding outside of job "
		     "step allocation for task[%u]", task_id);
		fprintf(stderr, "Requested cpu_bind option outside of job "
			"step allocation for task[%u]\n", task_id);
	}
#if HWLOC_API_VERSION >= 0x00020000
	hwloc_bitmap_free(allowed_cpuset);
#endif
}

#endif

extern int task_cgroup_cpuset_init(void)
{
	cgroup_g_initialize(CG_CPUS);
	return SLURM_SUCCESS;
}

extern int task_cgroup_cpuset_fini(void)
{
	return cgroup_g_step_destroy(CG_CPUS);
}

extern int task_cgroup_cpuset_create(stepd_step_rec_t *job)
{
	cgroup_limits_t limits, *root_limits = NULL;
	char *job_alloc_cpus = NULL;
	char *step_alloc_cpus = NULL;
	int rc = SLURM_SUCCESS;

	/* First create the cpuset hierarchy for this job */
	if ((rc = cgroup_g_step_create(CG_CPUS, job)) != SLURM_SUCCESS)
		return rc;

	/* Then constrain the user/job/step to the required cores/cpus */

	/* build job and job steps allocated cores lists */
	debug("job abstract cores are '%s'", job->job_alloc_cores);
	debug("step abstract cores are '%s'", job->step_alloc_cores);

	if (xcpuinfo_abs_to_mac(job->job_alloc_cores,
				&job_alloc_cpus) != SLURM_SUCCESS) {
		error("unable to build job physical cores");
		goto endit;
	}
	if (xcpuinfo_abs_to_mac(job->step_alloc_cores,
				&step_alloc_cpus) != SLURM_SUCCESS) {
		error("unable to build step physical cores");
		goto endit;
	}
	debug("job physical CPUs are '%s'", job_alloc_cpus);
	debug("step physical CPUs are '%s'", step_alloc_cpus);

	/*
	 * check that user's cpuset cgroup is consistent and add the job's CPUs
	 */
	root_limits = cgroup_g_root_constrain_get(CG_CPUS);

	if (!root_limits)
		goto endit;

	memset(&limits, 0, sizeof(limits));
	limits.allow_mems = root_limits->allow_mems;

	/* User constrain */
	limits.allow_cores = xstrdup_printf(
		"%s,%s", job_alloc_cpus, root_limits->allow_cores);
	rc = cgroup_g_user_constrain_set(CG_CPUS, job, &limits);
	xfree(limits.allow_cores);
	if (rc != SLURM_SUCCESS)
		goto endit;

	/* Job constrain */
	limits.allow_cores = job_alloc_cpus;
	rc = cgroup_g_job_constrain_set(CG_CPUS, job, &limits);
	if (rc != SLURM_SUCCESS)
		goto endit;

	/* Step constrain */
	limits.allow_cores = step_alloc_cpus;
	rc = cgroup_g_step_constrain_set(CG_CPUS, job, &limits);
	if (rc != SLURM_SUCCESS)
		goto endit;

	/* validate the requested cpu frequency and set it */
	cpu_freq_cgroup_validate(job, step_alloc_cpus);

endit:
	xfree(job_alloc_cpus);
	xfree(step_alloc_cpus);
	cgroup_free_limits(root_limits);
	return rc;
}

/* affinity should be set using sched_setaffinity to not force */
/* user to have to play with the cgroup hierarchy to modify it */
extern int task_cgroup_cpuset_set_task_affinity(stepd_step_rec_t *job,
						uint32_t taskid)
{
	int fstatus = SLURM_ERROR;

#ifndef HAVE_HWLOC

	error("plugin not compiled with hwloc support, "
	      "skipping affinity.");
	return fstatus;

#else
	char mstr[1 + CPU_SETSIZE / 4];
	cpu_bind_type_t bind_type;
	cpu_set_t ts;
	hwloc_obj_t obj;
	hwloc_obj_type_t socket_or_node;
	hwloc_topology_t topology;
	hwloc_bitmap_t cpuset;
	hwloc_obj_type_t hwtype;
	hwloc_obj_type_t req_hwtype;
	int bind_verbose = 0;
	int rc = SLURM_SUCCESS;
	pid_t    pid = job->task[taskid]->pid;
	size_t tssize;
	uint32_t nldoms;
	uint32_t nsockets;
	uint32_t ncores;
	uint32_t npus;
	uint32_t nobj;
	uint32_t jntasks = job->node_tasks;
	uint32_t jnpus;
	int spec_threads = 0;

	hwloc_obj_type_t socket_type = HWLOC_OBJ_SOCKET;
#if HWLOC_API_VERSION >= 0x00020000
	if (xstrcasestr(slurm_conf.slurmd_params, "l3cache_as_socket"))
		socket_type = HWLOC_OBJ_L3CACHE;
#endif

	/* Allocate and initialize hwloc objects */
	hwloc_topology_init(&topology);

	xassert(conf->hwloc_xml);
	xcpuinfo_hwloc_topo_load(&topology, conf->hwloc_xml, false);

	cpuset = hwloc_bitmap_alloc();
#if HWLOC_API_VERSION >= 0x00020000
	global_allowed_cpuset = hwloc_bitmap_alloc();
	(void) hwloc_bitmap_copy(global_allowed_cpuset,
				 hwloc_topology_get_allowed_cpuset(topology));
#endif

	if (job->batch) {
		jnpus = job->cpus;
		job->cpus_per_task = job->cpus;
	} else
		jnpus = jntasks * job->cpus_per_task;

	bind_type = job->cpu_bind_type;
	if ((slurm_conf.task_plugin_param & CPU_BIND_VERBOSE) ||
	    (bind_type & CPU_BIND_VERBOSE))
		bind_verbose = 1 ;

	if ( hwloc_get_type_depth(topology, HWLOC_OBJ_NODE) >
	     hwloc_get_type_depth(topology, HWLOC_OBJ_SOCKET) ) {
		/* One socket contains multiple NUMA-nodes
		 * like AMD Opteron 6000 series etc.
		 * In such case, use NUMA-node instead of socket. */
		socket_or_node = HWLOC_OBJ_NODE;
	} else {
		socket_or_node = socket_type;
	}

	if (bind_type & CPU_BIND_NONE) {
		if (bind_verbose)
			info("task[%u] is requesting no affinity",
			     taskid);
		return 0;
	} else if (bind_type & CPU_BIND_TO_THREADS) {
		if (bind_verbose)
			info("task[%u] is requesting "
			     "thread level binding",taskid);
		req_hwtype = HWLOC_OBJ_PU;
	} else if (bind_type & CPU_BIND_TO_CORES) {
		if (bind_verbose)
			info("task[%u] is requesting "
			     "core level binding",taskid);
		req_hwtype = HWLOC_OBJ_CORE;
	} else if (bind_type & CPU_BIND_TO_SOCKETS) {
		if (bind_verbose)
			info("task[%u] is requesting "
			     "socket level binding",taskid);
		req_hwtype = socket_or_node;
	} else if (bind_type & CPU_BIND_TO_LDOMS) {
		if (bind_verbose)
			info("task[%u] is requesting "
			     "ldom level binding",taskid);
		req_hwtype = HWLOC_OBJ_NODE;
	} else if (bind_type & CPU_BIND_TO_BOARDS) {
		if (bind_verbose)
			info("task[%u] is requesting "
			     "board level binding",taskid);
		req_hwtype = HWLOC_OBJ_GROUP;
	} else if (bind_type & bind_mode_ldom) {
		req_hwtype = HWLOC_OBJ_NODE;
	} else {
		if (bind_verbose)
			info("task[%u] using core level binding"
			     " by default",taskid);
		req_hwtype = HWLOC_OBJ_CORE;
	}

	/*
	 * Perform the topology detection. It will only get allowed PUs.
	 * Detect in the same time the granularity to use for binding.
	 * The granularity can be relaxed from threads to cores if enough
	 * cores are available as with hyperthread support, ntasks-per-core
	 * param can let us have access to more threads per core for each
	 * task
	 * Revert back to machine granularity if no finer-grained granularity
	 * matching the request is found. This will result in no affinity
	 * applied.
	 * The detected granularity will be used to find where to best place
	 * the task, then the cpu_bind option will be used to relax the
	 * affinity constraint and use more PUs. (i.e. use a core granularity
	 * to dispatch the tasks across the sockets and then provide access
	 * to each task to the cores of its socket.)
	 */
	npus = (uint32_t) hwloc_get_nbobjs_by_type(topology,
						   HWLOC_OBJ_PU);
	ncores = (uint32_t) hwloc_get_nbobjs_by_type(topology,
						     HWLOC_OBJ_CORE);
	nsockets = (uint32_t) hwloc_get_nbobjs_by_type(topology,
						       socket_or_node);
	nldoms = (uint32_t) hwloc_get_nbobjs_by_type(topology,
						     HWLOC_OBJ_NODE);
	//info("PU:%d CORE:%d SOCK:%d LDOM:%d", npus, ncores, nsockets, nldoms);

	hwtype = HWLOC_OBJ_MACHINE;
	nobj = 1;
	if ((job->job_core_spec != NO_VAL16) &&
	    (job->job_core_spec &  CORE_SPEC_THREAD)  &&
	    (job->job_core_spec != CORE_SPEC_THREAD)) {
		spec_threads = job->job_core_spec & (~CORE_SPEC_THREAD);
	}

	/* Set this to PU but realise it could be overridden later if we can
	 * fill up a core.
	 */
	if (npus >= (jnpus + spec_threads)) {
		hwtype = HWLOC_OBJ_PU;
		nobj = npus;
	}

	/* Force to bind to Threads */
	if (bind_type & CPU_BIND_TO_THREADS) {
		hwtype = HWLOC_OBJ_PU;
		nobj = npus;
	} else if (ncores >= jnpus || bind_type & CPU_BIND_TO_CORES) {
		hwtype = HWLOC_OBJ_CORE;
		nobj = ncores;
	}

	if (nsockets >= jntasks &&
	    bind_type & CPU_BIND_TO_SOCKETS) {
		hwtype = socket_or_node;
		nobj = nsockets;
	}
	/*
	 * HWLOC returns all the NUMA nodes available regardless of the
	 * number of underlying sockets available (regardless of the allowed
	 * resources). So there is no guarantee that each ldom will be populated
	 * with usable sockets. So add a simple check that at least ensure that
	 * we have as many sockets as ldoms before moving to ldoms granularity
	 */
	if (nldoms >= jntasks &&
	    nsockets >= nldoms &&
	    bind_type & (CPU_BIND_TO_LDOMS | bind_mode_ldom)) {
		hwtype = HWLOC_OBJ_NODE;
		nobj = nldoms;
	}

	/*
	 * If not enough objects to do the job, revert to no affinity mode
	 */
	if (hwloc_compare_types(hwtype, HWLOC_OBJ_MACHINE) == 0) {
		info("task[%u] disabling affinity because of %s "
		     "granularity",taskid, hwloc_obj_type_string(hwtype));
		if (job->cpu_bind_type & CPU_BIND_VERBOSE)
			fprintf(stderr,"task[%u] disabling "
				"affinity because of %s granularity\n",
				taskid, hwloc_obj_type_string(hwtype));
	} else if ((hwloc_compare_types(hwtype, HWLOC_OBJ_CORE) >= 0) &&
		   (nobj < jnpus)) {
		info("task[%u] not enough %s objects (%d < %d), "
		     "disabling affinity",
		     taskid, hwloc_obj_type_string(hwtype), nobj, jnpus);
		if (job->cpu_bind_type & CPU_BIND_VERBOSE)
			fprintf(stderr, "task[%u] not enough %s "
				"objects (%d < %d), disabling affinity\n",
				taskid, hwloc_obj_type_string(hwtype),
				nobj, jnpus);
	} else if (bind_type & bind_mode) {
		/*
		 * Explicit binding mode specified by the user
		 * Bind the taskid in accordance with the specified mode
		 */
		obj = hwloc_get_obj_by_type(topology, HWLOC_OBJ_MACHINE, 0);
		if (bind_verbose) {
			info("task[%u] is requesting "
			     "explicit binding mode", taskid);
		}
		_get_sched_cpuset(topology, hwtype, req_hwtype, &ts, job);
		tssize = sizeof(cpu_set_t);
		fstatus = SLURM_SUCCESS;
		_validate_mask(taskid, obj, &ts);
		if ((rc = sched_setaffinity(pid, tssize, &ts))) {
			error("task[%u] unable to set "
			      "mask 0x%s", taskid,
			      task_cpuset_to_str(&ts, mstr));
			error("sched_setaffinity rc = %d", rc);
			fstatus = SLURM_ERROR;
		} else if (bind_verbose) {
			info("task[%u] mask 0x%s",
			     taskid, task_cpuset_to_str(&ts, mstr));
		}
		task_slurm_chkaffinity(&ts, job, rc);
	} else {
		/*
		 * Bind the detected object to the taskid, respecting the
		 * granularity, using the designated or default distribution
		 * method (block or cyclic).
		 */
		char *str = NULL;

		if (bind_verbose) {
			info("task[%u] using %s granularity dist %u",
			     taskid, hwloc_obj_type_string(hwtype),
			     job->task_dist);
		}

		/*
		 * See srun man page for detailed information on --distribution
		 * option.
		 *
		 * You can see the equivalent code for the
		 * task/affinity plugin in
		 * src/plugins/task/affinity/dist_tasks.c, around line 368
		 */
		switch (job->task_dist & SLURM_DIST_NODESOCKMASK) {
		case SLURM_DIST_BLOCK_BLOCK:
		case SLURM_DIST_CYCLIC_BLOCK:
		case SLURM_DIST_PLANE:
			/* tasks are distributed in blocks within a plane */
			_task_cgroup_cpuset_dist_block(topology,
				hwtype, req_hwtype,
				nobj, job, bind_verbose, cpuset);
			break;
		case SLURM_DIST_ARBITRARY:
		case SLURM_DIST_BLOCK:
		case SLURM_DIST_CYCLIC:
		case SLURM_DIST_UNKNOWN:
			if (slurm_conf.select_type_param
			    & CR_CORE_DEFAULT_DIST_BLOCK) {
				_task_cgroup_cpuset_dist_block(topology,
					hwtype, req_hwtype,
					nobj, job, bind_verbose, cpuset);
				break;
			}
			/*
			 * We want to fall through here if we aren't doing a
			 * default dist block.
			 */
		default:
			_task_cgroup_cpuset_dist_cyclic(topology,
				hwtype, req_hwtype,
				job, bind_verbose, cpuset);
			break;
		}

		hwloc_bitmap_asprintf(&str, cpuset);

		tssize = sizeof(cpu_set_t);
		if (hwloc_cpuset_to_glibc_sched_affinity(topology, cpuset,
							 &ts, tssize) == 0) {
			fstatus = SLURM_SUCCESS;
			if ((rc = sched_setaffinity(pid, tssize, &ts))) {
				error("task[%u] unable to set "
				      "taskset '%s'", taskid, str);
				fstatus = SLURM_ERROR;
			} else if (bind_verbose) {
				info("task[%u] set taskset '%s'",
				     taskid, str);
			}
			task_slurm_chkaffinity(&ts, job, rc);
		} else {
			error("task[%u] unable to build "
			      "taskset '%s'",taskid,str);
			fstatus = SLURM_ERROR;
		}

		if (str)
			free(str);
	}

	/* Destroy hwloc objects */
	hwloc_bitmap_free(cpuset);
	hwloc_topology_destroy(topology);
#if HWLOC_API_VERSION >= 0x00020000
	hwloc_bitmap_free(global_allowed_cpuset);
#endif

	return fstatus;
#endif

}

/*
 * Keep track a of a pid.
 */
extern int task_cgroup_cpuset_add_pid(pid_t pid)
{
	return cgroup_g_step_addto(CG_CPUS, &pid, 1);
}

#endif
