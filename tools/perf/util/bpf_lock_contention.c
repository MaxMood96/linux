// SPDX-License-Identifier: GPL-2.0
#include "util/cgroup.h"
#include "util/debug.h"
#include "util/evlist.h"
#include "util/hashmap.h"
#include "util/machine.h"
#include "util/map.h"
#include "util/symbol.h"
#include "util/target.h"
#include "util/thread.h"
#include "util/thread_map.h"
#include "util/lock-contention.h"
#include <linux/zalloc.h>
#include <linux/string.h>
#include <api/fs/fs.h>
#include <bpf/bpf.h>
#include <bpf/btf.h>
#include <inttypes.h>

#include "bpf_skel/lock_contention.skel.h"
#include "bpf_skel/lock_data.h"

static struct lock_contention_bpf *skel;
static bool has_slab_iter;
static struct hashmap slab_hash;

static size_t slab_cache_hash(long key, void *ctx __maybe_unused)
{
	return key;
}

static bool slab_cache_equal(long key1, long key2, void *ctx __maybe_unused)
{
	return key1 == key2;
}

static void check_slab_cache_iter(struct lock_contention *con)
{
	s32 ret;

	hashmap__init(&slab_hash, slab_cache_hash, slab_cache_equal, /*ctx=*/NULL);

	con->btf = btf__load_vmlinux_btf();
	if (con->btf == NULL) {
		pr_debug("BTF loading failed: %s\n", strerror(errno));
		return;
	}

	ret = btf__find_by_name_kind(con->btf, "bpf_iter__kmem_cache", BTF_KIND_STRUCT);
	if (ret < 0) {
		bpf_program__set_autoload(skel->progs.slab_cache_iter, false);
		pr_debug("slab cache iterator is not available: %d\n", ret);
		return;
	}

	has_slab_iter = true;

	bpf_map__set_max_entries(skel->maps.slab_caches, con->map_nr_entries);
}

static void run_slab_cache_iter(void)
{
	int fd;
	char buf[256];
	long key, *prev_key;

	if (!has_slab_iter)
		return;

	fd = bpf_iter_create(bpf_link__fd(skel->links.slab_cache_iter));
	if (fd < 0) {
		pr_debug("cannot create slab cache iter: %d\n", fd);
		return;
	}

	/* This will run the bpf program */
	while (read(fd, buf, sizeof(buf)) > 0)
		continue;

	close(fd);

	/* Read the slab cache map and build a hash with IDs */
	fd = bpf_map__fd(skel->maps.slab_caches);
	prev_key = NULL;
	while (!bpf_map_get_next_key(fd, prev_key, &key)) {
		struct slab_cache_data *data;

		data = malloc(sizeof(*data));
		if (data == NULL)
			break;

		if (bpf_map_lookup_elem(fd, &key, data) < 0)
			break;

		hashmap__add(&slab_hash, data->id, data);
		prev_key = &key;
	}
}

static void exit_slab_cache_iter(void)
{
	struct hashmap_entry *cur;
	unsigned bkt;

	hashmap__for_each_entry(&slab_hash, cur, bkt)
		free(cur->pvalue);

	hashmap__clear(&slab_hash);
}

static void init_numa_data(struct lock_contention *con)
{
	struct symbol *sym;
	struct map *kmap;
	char *buf = NULL, *p;
	size_t len;
	long last = -1;
	int ret;

	/*
	 * 'struct zone' is embedded in 'struct pglist_data' as an array.
	 * As we may not have full information of the struct zone in the
	 * (fake) vmlinux.h, let's get the actual size from BTF.
	 */
	ret = btf__find_by_name_kind(con->btf, "zone", BTF_KIND_STRUCT);
	if (ret < 0) {
		pr_debug("cannot get type of struct zone: %d\n", ret);
		return;
	}

	ret = btf__resolve_size(con->btf, ret);
	if (ret < 0) {
		pr_debug("cannot get size of struct zone: %d\n", ret);
		return;
	}
	skel->rodata->sizeof_zone = ret;

	/* UMA system doesn't have 'node_data[]' - just use contig_page_data. */
	sym = machine__find_kernel_symbol_by_name(con->machine,
						  "contig_page_data",
						  &kmap);
	if (sym) {
		skel->rodata->contig_page_data_addr = map__unmap_ip(kmap, sym->start);
		map__put(kmap);
		return;
	}

	/*
	 * The 'node_data' is an array of pointers to struct pglist_data.
	 * It needs to follow the pointer for each node in BPF to get the
	 * address of struct pglist_data and its zones.
	 */
	sym = machine__find_kernel_symbol_by_name(con->machine,
						  "node_data",
						  &kmap);
	if (sym == NULL)
		return;

	skel->rodata->node_data_addr = map__unmap_ip(kmap, sym->start);
	map__put(kmap);

	/* get the number of online nodes using the last node number + 1 */
	ret = sysfs__read_str("devices/system/node/online", &buf, &len);
	if (ret < 0) {
		pr_debug("failed to read online node: %d\n", ret);
		return;
	}

	p = buf;
	while (p && *p) {
		last = strtol(p, &p, 0);

		if (p && (*p == ',' || *p == '-' || *p == '\n'))
			p++;
	}
	skel->rodata->nr_nodes = last + 1;
	free(buf);
}

int lock_contention_prepare(struct lock_contention *con)
{
	int i, fd;
	int ncpus = 1, ntasks = 1, ntypes = 1, naddrs = 1, ncgrps = 1, nslabs = 1;
	struct evlist *evlist = con->evlist;
	struct target *target = con->target;

	skel = lock_contention_bpf__open();
	if (!skel) {
		pr_err("Failed to open lock-contention BPF skeleton\n");
		return -1;
	}

	bpf_map__set_value_size(skel->maps.stacks, con->max_stack * sizeof(u64));
	bpf_map__set_max_entries(skel->maps.lock_stat, con->map_nr_entries);
	bpf_map__set_max_entries(skel->maps.tstamp, con->map_nr_entries);

	if (con->aggr_mode == LOCK_AGGR_TASK)
		bpf_map__set_max_entries(skel->maps.task_data, con->map_nr_entries);
	else
		bpf_map__set_max_entries(skel->maps.task_data, 1);

	if (con->save_callstack) {
		bpf_map__set_max_entries(skel->maps.stacks, con->map_nr_entries);
		if (con->owner) {
			bpf_map__set_value_size(skel->maps.stack_buf, con->max_stack * sizeof(u64));
			bpf_map__set_key_size(skel->maps.owner_stacks,
						con->max_stack * sizeof(u64));
			bpf_map__set_max_entries(skel->maps.owner_stacks, con->map_nr_entries);
			bpf_map__set_max_entries(skel->maps.owner_data, con->map_nr_entries);
			bpf_map__set_max_entries(skel->maps.owner_stat, con->map_nr_entries);
			skel->rodata->max_stack = con->max_stack;
		}
	} else {
		bpf_map__set_max_entries(skel->maps.stacks, 1);
	}

	if (target__has_cpu(target)) {
		skel->rodata->has_cpu = 1;
		ncpus = perf_cpu_map__nr(evlist->core.user_requested_cpus);
	}
	if (target__has_task(target)) {
		skel->rodata->has_task = 1;
		ntasks = perf_thread_map__nr(evlist->core.threads);
	}
	if (con->filters->nr_types) {
		skel->rodata->has_type = 1;
		ntypes = con->filters->nr_types;
	}
	if (con->filters->nr_cgrps) {
		skel->rodata->has_cgroup = 1;
		ncgrps = con->filters->nr_cgrps;
	}

	/* resolve lock name filters to addr */
	if (con->filters->nr_syms) {
		struct symbol *sym;
		struct map *kmap;
		unsigned long *addrs;

		for (i = 0; i < con->filters->nr_syms; i++) {
			sym = machine__find_kernel_symbol_by_name(con->machine,
								  con->filters->syms[i],
								  &kmap);
			if (sym == NULL) {
				pr_warning("ignore unknown symbol: %s\n",
					   con->filters->syms[i]);
				continue;
			}

			addrs = realloc(con->filters->addrs,
					(con->filters->nr_addrs + 1) * sizeof(*addrs));
			if (addrs == NULL) {
				pr_warning("memory allocation failure\n");
				continue;
			}

			addrs[con->filters->nr_addrs++] = map__unmap_ip(kmap, sym->start);
			con->filters->addrs = addrs;
		}
		naddrs = con->filters->nr_addrs;
		skel->rodata->has_addr = 1;
	}

	/* resolve lock name in delays */
	if (con->nr_delays) {
		struct symbol *sym;
		struct map *kmap;

		for (i = 0; i < con->nr_delays; i++) {
			sym = machine__find_kernel_symbol_by_name(con->machine,
								  con->delays[i].sym,
								  &kmap);
			if (sym == NULL) {
				pr_warning("ignore unknown symbol: %s\n",
					   con->delays[i].sym);
				continue;
			}

			con->delays[i].addr = map__unmap_ip(kmap, sym->start);
		}
		skel->rodata->lock_delay = 1;
		bpf_map__set_max_entries(skel->maps.lock_delays, con->nr_delays);
	}

	bpf_map__set_max_entries(skel->maps.cpu_filter, ncpus);
	bpf_map__set_max_entries(skel->maps.task_filter, ntasks);
	bpf_map__set_max_entries(skel->maps.type_filter, ntypes);
	bpf_map__set_max_entries(skel->maps.addr_filter, naddrs);
	bpf_map__set_max_entries(skel->maps.cgroup_filter, ncgrps);

	skel->rodata->stack_skip = con->stack_skip;
	skel->rodata->aggr_mode = con->aggr_mode;
	skel->rodata->needs_callstack = con->save_callstack;
	skel->rodata->lock_owner = con->owner;

	if (con->aggr_mode == LOCK_AGGR_CGROUP || con->filters->nr_cgrps) {
		if (cgroup_is_v2("perf_event"))
			skel->rodata->use_cgroup_v2 = 1;
	}

	check_slab_cache_iter(con);

	if (con->filters->nr_slabs && has_slab_iter) {
		skel->rodata->has_slab = 1;
		nslabs = con->filters->nr_slabs;
	}

	bpf_map__set_max_entries(skel->maps.slab_filter, nslabs);

	init_numa_data(con);

	if (lock_contention_bpf__load(skel) < 0) {
		pr_err("Failed to load lock-contention BPF skeleton\n");
		return -1;
	}

	if (target__has_cpu(target)) {
		u32 cpu;
		u8 val = 1;

		fd = bpf_map__fd(skel->maps.cpu_filter);

		for (i = 0; i < ncpus; i++) {
			cpu = perf_cpu_map__cpu(evlist->core.user_requested_cpus, i).cpu;
			bpf_map_update_elem(fd, &cpu, &val, BPF_ANY);
		}
	}

	if (target__has_task(target)) {
		u32 pid;
		u8 val = 1;

		fd = bpf_map__fd(skel->maps.task_filter);

		for (i = 0; i < ntasks; i++) {
			pid = perf_thread_map__pid(evlist->core.threads, i);
			bpf_map_update_elem(fd, &pid, &val, BPF_ANY);
		}
	}

	if (target__none(target) && evlist->workload.pid > 0) {
		u32 pid = evlist->workload.pid;
		u8 val = 1;

		fd = bpf_map__fd(skel->maps.task_filter);
		bpf_map_update_elem(fd, &pid, &val, BPF_ANY);
	}

	if (con->filters->nr_types) {
		u8 val = 1;

		fd = bpf_map__fd(skel->maps.type_filter);

		for (i = 0; i < con->filters->nr_types; i++)
			bpf_map_update_elem(fd, &con->filters->types[i], &val, BPF_ANY);
	}

	if (con->filters->nr_addrs) {
		u8 val = 1;

		fd = bpf_map__fd(skel->maps.addr_filter);

		for (i = 0; i < con->filters->nr_addrs; i++)
			bpf_map_update_elem(fd, &con->filters->addrs[i], &val, BPF_ANY);
	}

	if (con->filters->nr_cgrps) {
		u8 val = 1;

		fd = bpf_map__fd(skel->maps.cgroup_filter);

		for (i = 0; i < con->filters->nr_cgrps; i++)
			bpf_map_update_elem(fd, &con->filters->cgrps[i], &val, BPF_ANY);
	}

	if (con->nr_delays) {
		fd = bpf_map__fd(skel->maps.lock_delays);

		for (i = 0; i < con->nr_delays; i++)
			bpf_map_update_elem(fd, &con->delays[i].addr, &con->delays[i].time, BPF_ANY);
	}

	if (con->aggr_mode == LOCK_AGGR_CGROUP)
		read_all_cgroups(&con->cgroups);

	bpf_program__set_autoload(skel->progs.collect_lock_syms, false);

	lock_contention_bpf__attach(skel);

	/* run the slab iterator after attaching */
	run_slab_cache_iter();

	if (con->filters->nr_slabs) {
		u8 val = 1;
		int cache_fd;
		long key, *prev_key;

		fd = bpf_map__fd(skel->maps.slab_filter);

		/* Read the slab cache map and build a hash with its address */
		cache_fd = bpf_map__fd(skel->maps.slab_caches);
		prev_key = NULL;
		while (!bpf_map_get_next_key(cache_fd, prev_key, &key)) {
			struct slab_cache_data data;

			if (bpf_map_lookup_elem(cache_fd, &key, &data) < 0)
				break;

			for (i = 0; i < con->filters->nr_slabs; i++) {
				if (!strcmp(con->filters->slabs[i], data.name)) {
					bpf_map_update_elem(fd, &key, &val, BPF_ANY);
					break;
				}
			}
			prev_key = &key;
		}
	}

	return 0;
}

/*
 * Run the BPF program directly using BPF_PROG_TEST_RUN to update the end
 * timestamp in ktime so that it can calculate delta easily.
 */
static void mark_end_timestamp(void)
{
	DECLARE_LIBBPF_OPTS(bpf_test_run_opts, opts,
		.flags = BPF_F_TEST_RUN_ON_CPU,
	);
	int prog_fd = bpf_program__fd(skel->progs.end_timestamp);

	bpf_prog_test_run_opts(prog_fd, &opts);
}

static void update_lock_stat(int map_fd, int pid, u64 end_ts,
			     enum lock_aggr_mode aggr_mode,
			     struct tstamp_data *ts_data)
{
	u64 delta;
	struct contention_key stat_key = {};
	struct contention_data stat_data;

	if (ts_data->timestamp >= end_ts)
		return;

	delta = end_ts - ts_data->timestamp;

	switch (aggr_mode) {
	case LOCK_AGGR_CALLER:
		stat_key.stack_id = ts_data->stack_id;
		break;
	case LOCK_AGGR_TASK:
		stat_key.pid = pid;
		break;
	case LOCK_AGGR_ADDR:
		stat_key.lock_addr_or_cgroup = ts_data->lock;
		break;
	case LOCK_AGGR_CGROUP:
		/* TODO */
		return;
	default:
		return;
	}

	if (bpf_map_lookup_elem(map_fd, &stat_key, &stat_data) < 0)
		return;

	stat_data.total_time += delta;
	stat_data.count++;

	if (delta > stat_data.max_time)
		stat_data.max_time = delta;
	if (delta < stat_data.min_time)
		stat_data.min_time = delta;

	bpf_map_update_elem(map_fd, &stat_key, &stat_data, BPF_EXIST);
}

/*
 * Account entries in the tstamp map (which didn't see the corresponding
 * lock:contention_end tracepoint) using end_ts.
 */
static void account_end_timestamp(struct lock_contention *con)
{
	int ts_fd, stat_fd;
	int *prev_key, key;
	u64 end_ts = skel->bss->end_ts;
	int total_cpus;
	enum lock_aggr_mode aggr_mode = con->aggr_mode;
	struct tstamp_data ts_data, *cpu_data;

	/* Iterate per-task tstamp map (key = TID) */
	ts_fd = bpf_map__fd(skel->maps.tstamp);
	stat_fd = bpf_map__fd(skel->maps.lock_stat);

	prev_key = NULL;
	while (!bpf_map_get_next_key(ts_fd, prev_key, &key)) {
		if (bpf_map_lookup_elem(ts_fd, &key, &ts_data) == 0) {
			int pid = key;

			if (aggr_mode == LOCK_AGGR_TASK && con->owner)
				pid = ts_data.flags;

			update_lock_stat(stat_fd, pid, end_ts, aggr_mode,
					 &ts_data);
		}

		prev_key = &key;
	}

	/* Now it'll check per-cpu tstamp map which doesn't have TID. */
	if (aggr_mode == LOCK_AGGR_TASK || aggr_mode == LOCK_AGGR_CGROUP)
		return;

	total_cpus = cpu__max_cpu().cpu;
	ts_fd = bpf_map__fd(skel->maps.tstamp_cpu);

	cpu_data = calloc(total_cpus, sizeof(*cpu_data));
	if (cpu_data == NULL)
		return;

	prev_key = NULL;
	while (!bpf_map_get_next_key(ts_fd, prev_key, &key)) {
		if (bpf_map_lookup_elem(ts_fd, &key, cpu_data) < 0)
			goto next;

		for (int i = 0; i < total_cpus; i++) {
			if (cpu_data[i].lock == 0)
				continue;

			update_lock_stat(stat_fd, -1, end_ts, aggr_mode,
					 &cpu_data[i]);
		}

next:
		prev_key = &key;
	}
	free(cpu_data);
}

int lock_contention_start(void)
{
	skel->bss->enabled = 1;
	return 0;
}

int lock_contention_stop(void)
{
	skel->bss->enabled = 0;
	mark_end_timestamp();
	return 0;
}

static const char *lock_contention_get_name(struct lock_contention *con,
					    struct contention_key *key,
					    u64 *stack_trace, u32 flags)
{
	int idx = 0;
	u64 addr;
	static char name_buf[KSYM_NAME_LEN];
	struct symbol *sym;
	struct map *kmap;
	struct machine *machine = con->machine;

	if (con->aggr_mode == LOCK_AGGR_TASK) {
		struct contention_task_data task;
		int pid = key->pid;
		int task_fd = bpf_map__fd(skel->maps.task_data);

		/* do not update idle comm which contains CPU number */
		if (pid) {
			struct thread *t = machine__findnew_thread(machine, /*pid=*/-1, pid);

			if (t != NULL &&
			    !bpf_map_lookup_elem(task_fd, &pid, &task) &&
			    thread__set_comm(t, task.comm, /*timestamp=*/0)) {
				snprintf(name_buf, sizeof(name_buf), "%s", task.comm);
				return name_buf;
			}
		}
		return "";
	}

	if (con->aggr_mode == LOCK_AGGR_ADDR) {
		int lock_fd = bpf_map__fd(skel->maps.lock_syms);
		struct slab_cache_data *slab_data;

		/* per-process locks set upper bits of the flags */
		if (flags & LCD_F_MMAP_LOCK)
			return "mmap_lock";
		if (flags & LCD_F_SIGHAND_LOCK)
			return "siglock";

		/* global locks with symbols */
		sym = machine__find_kernel_symbol(machine, key->lock_addr_or_cgroup, &kmap);
		if (sym)
			return sym->name;

		/* try semi-global locks collected separately */
		if (!bpf_map_lookup_elem(lock_fd, &key->lock_addr_or_cgroup, &flags)) {
			if (flags == LOCK_CLASS_RQLOCK)
				return "rq_lock";
		}

		if (!bpf_map_lookup_elem(lock_fd, &key->lock_addr_or_cgroup, &flags)) {
			if (flags == LOCK_CLASS_ZONE_LOCK)
				return "zone_lock";
		}

		/* look slab_hash for dynamic locks in a slab object */
		if (hashmap__find(&slab_hash, flags & LCB_F_SLAB_ID_MASK, &slab_data)) {
			snprintf(name_buf, sizeof(name_buf), "&%s", slab_data->name);
			return name_buf;
		}

		return "";
	}

	if (con->aggr_mode == LOCK_AGGR_CGROUP) {
		u64 cgrp_id = key->lock_addr_or_cgroup;
		struct cgroup *cgrp = __cgroup__find(&con->cgroups, cgrp_id);

		if (cgrp)
			return cgrp->name;

		snprintf(name_buf, sizeof(name_buf), "cgroup:%" PRIu64 "", cgrp_id);
		return name_buf;
	}

	/* LOCK_AGGR_CALLER: skip lock internal functions */
	while (machine__is_lock_function(machine, stack_trace[idx]) &&
	       idx < con->max_stack - 1)
		idx++;

	addr = stack_trace[idx];
	sym = machine__find_kernel_symbol(machine, addr, &kmap);

	if (sym) {
		unsigned long offset;

		offset = map__map_ip(kmap, addr) - sym->start;

		if (offset == 0)
			return sym->name;

		snprintf(name_buf, sizeof(name_buf), "%s+%#lx", sym->name, offset);
	} else {
		snprintf(name_buf, sizeof(name_buf), "%#lx", (unsigned long)addr);
	}

	return name_buf;
}

struct lock_stat *pop_owner_stack_trace(struct lock_contention *con)
{
	int stacks_fd, stat_fd;
	u64 *stack_trace = NULL;
	s32 stack_id;
	struct contention_key ckey = {};
	struct contention_data cdata = {};
	size_t stack_size = con->max_stack * sizeof(*stack_trace);
	struct lock_stat *st = NULL;

	stacks_fd = bpf_map__fd(skel->maps.owner_stacks);
	stat_fd = bpf_map__fd(skel->maps.owner_stat);
	if (!stacks_fd || !stat_fd)
		goto out_err;

	stack_trace = zalloc(stack_size);
	if (stack_trace == NULL)
		goto out_err;

	if (bpf_map_get_next_key(stacks_fd, NULL, stack_trace))
		goto out_err;

	bpf_map_lookup_elem(stacks_fd, stack_trace, &stack_id);
	ckey.stack_id = stack_id;
	bpf_map_lookup_elem(stat_fd, &ckey, &cdata);

	st = zalloc(sizeof(struct lock_stat));
	if (!st)
		goto out_err;

	st->name = strdup(stack_trace[0] ? lock_contention_get_name(con, NULL, stack_trace, 0) :
					   "unknown");
	if (!st->name)
		goto out_err;

	st->flags = cdata.flags;
	st->nr_contended = cdata.count;
	st->wait_time_total = cdata.total_time;
	st->wait_time_max = cdata.max_time;
	st->wait_time_min = cdata.min_time;
	st->callstack = stack_trace;

	if (cdata.count)
		st->avg_wait_time = cdata.total_time / cdata.count;

	bpf_map_delete_elem(stacks_fd, stack_trace);
	bpf_map_delete_elem(stat_fd, &ckey);

	return st;

out_err:
	free(stack_trace);
	free(st);

	return NULL;
}

int lock_contention_read(struct lock_contention *con)
{
	int fd, stack, err = 0;
	struct contention_key *prev_key, key = {};
	struct contention_data data = {};
	struct lock_stat *st = NULL;
	struct machine *machine = con->machine;
	u64 *stack_trace;
	size_t stack_size = con->max_stack * sizeof(*stack_trace);

	fd = bpf_map__fd(skel->maps.lock_stat);
	stack = bpf_map__fd(skel->maps.stacks);

	con->fails.task = skel->bss->task_fail;
	con->fails.stack = skel->bss->stack_fail;
	con->fails.time = skel->bss->time_fail;
	con->fails.data = skel->bss->data_fail;

	stack_trace = zalloc(stack_size);
	if (stack_trace == NULL)
		return -1;

	account_end_timestamp(con);

	if (con->aggr_mode == LOCK_AGGR_TASK) {
		struct thread *idle = machine__findnew_thread(machine,
								/*pid=*/0,
								/*tid=*/0);
		thread__set_comm(idle, "swapper", /*timestamp=*/0);
	}

	if (con->aggr_mode == LOCK_AGGR_ADDR) {
		DECLARE_LIBBPF_OPTS(bpf_test_run_opts, opts,
			.flags = BPF_F_TEST_RUN_ON_CPU,
		);
		int prog_fd = bpf_program__fd(skel->progs.collect_lock_syms);

		bpf_prog_test_run_opts(prog_fd, &opts);
	}

	/* make sure it loads the kernel map */
	maps__load_first(machine->kmaps);

	prev_key = NULL;
	while (!bpf_map_get_next_key(fd, prev_key, &key)) {
		s64 ls_key;
		const char *name;

		/* to handle errors in the loop body */
		err = -1;

		bpf_map_lookup_elem(fd, &key, &data);
		if (con->save_callstack) {
			bpf_map_lookup_elem(stack, &key.stack_id, stack_trace);

			if (!match_callstack_filter(machine, stack_trace, con->max_stack)) {
				con->nr_filtered += data.count;
				goto next;
			}
		}

		switch (con->aggr_mode) {
		case LOCK_AGGR_CALLER:
			ls_key = key.stack_id;
			break;
		case LOCK_AGGR_TASK:
			ls_key = key.pid;
			break;
		case LOCK_AGGR_ADDR:
		case LOCK_AGGR_CGROUP:
			ls_key = key.lock_addr_or_cgroup;
			break;
		default:
			goto next;
		}

		st = lock_stat_find(ls_key);
		if (st != NULL) {
			st->wait_time_total += data.total_time;
			if (st->wait_time_max < data.max_time)
				st->wait_time_max = data.max_time;
			if (st->wait_time_min > data.min_time)
				st->wait_time_min = data.min_time;

			st->nr_contended += data.count;
			if (st->nr_contended)
				st->avg_wait_time = st->wait_time_total / st->nr_contended;
			goto next;
		}

		name = lock_contention_get_name(con, &key, stack_trace, data.flags);
		st = lock_stat_findnew(ls_key, name, data.flags);
		if (st == NULL)
			break;

		st->nr_contended = data.count;
		st->wait_time_total = data.total_time;
		st->wait_time_max = data.max_time;
		st->wait_time_min = data.min_time;

		if (data.count)
			st->avg_wait_time = data.total_time / data.count;

		if (con->aggr_mode == LOCK_AGGR_CALLER && verbose > 0) {
			st->callstack = memdup(stack_trace, stack_size);
			if (st->callstack == NULL)
				break;
		}

next:
		prev_key = &key;

		/* we're fine now, reset the error */
		err = 0;
	}

	free(stack_trace);

	return err;
}

int lock_contention_finish(struct lock_contention *con)
{
	if (skel) {
		skel->bss->enabled = 0;
		lock_contention_bpf__destroy(skel);
	}

	while (!RB_EMPTY_ROOT(&con->cgroups)) {
		struct rb_node *node = rb_first(&con->cgroups);
		struct cgroup *cgrp = rb_entry(node, struct cgroup, node);

		rb_erase(node, &con->cgroups);
		cgroup__put(cgrp);
	}

	exit_slab_cache_iter();
	btf__free(con->btf);

	return 0;
}
