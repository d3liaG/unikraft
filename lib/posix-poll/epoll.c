/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2023, Unikraft GmbH and The Unikraft Authors.
 * Licensed under the BSD-3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 */

#include <errno.h>

#include <uk/arch/atomic.h>
#include <uk/assert.h>
#include <uk/essentials.h>
#include <uk/file/nops.h>
#include <uk/file/pollqueue.h>
#include <uk/posix-fd.h>
#include <uk/posix-fdtab.h>
#include <uk/posix-poll.h>
#include <uk/timeutil.h>
#include <uk/syscall.h>

#include <vfscore/file.h>
#include <vfscore/vnode.h>
#include <vfscore/eventpoll.h>


static const char EPOLL_VOLID[] = "epoll_vol";

#define EPOLL_EVENTS \
	(UKFD_POLLIN|UKFD_POLLOUT|EPOLLRDHUP|EPOLLPRI|UKFD_POLL_ALWAYS)
#define EPOLL_OPTS (EPOLLET|EPOLLONESHOT|EPOLLWAKEUP|EPOLLEXCLUSIVE)

#define events2mask(ev) (((ev) & EPOLL_EVENTS) | UKFD_POLL_ALWAYS)


struct epoll_legacy {
	struct eventpoll_cb ecb;
	const struct uk_file *epf;
	unsigned int mask;
	unsigned int revents;
	struct uk_list_head f_link;
};

struct epoll_entry {
	struct epoll_entry *next;
	int legacy;
	int fd;
	union {
		const struct uk_file *f;
		struct vfscore_file *vf;
	};
	struct epoll_event event;
	union {
		struct {
			struct uk_poll_chain tick;
			uk_pollevent revents;
		};
		struct epoll_legacy legacy_cb;
	};
};
#define IS_EDGEPOLL(ent) (!!((ent)->event.events & EPOLLET))
#define IS_ONESHOT(ent)  (!!((ent)->event.events & EPOLLONESHOT))


struct epoll_alloc {
	struct uk_alloc *alloc;
	struct uk_file f;
	uk_file_refcnt frefcnt;
	struct uk_file_state fstate;
	struct epoll_entry *list;
};


static void epoll_unregister_entry(struct epoll_entry *ent)
{
	if (ent->legacy) {
		if (ent->legacy_cb.ecb.unregister)
			ent->legacy_cb.ecb.unregister(&ent->legacy_cb.ecb);
		uk_list_del(&ent->legacy_cb.f_link);
	} else {
		uk_pollq_unregister(&ent->f->state->pollq, &ent->tick);
		uk_file_release_weak(ent->f);
	}
}


static void epoll_event_callback(uk_pollevent set,
				 enum uk_poll_chain_op op,
				 struct uk_poll_chain *tick)
{
	if (op == UK_POLL_CHAINOP_SET) {
		struct epoll_entry *ent = __containerof(
			tick, struct epoll_entry, tick);
		struct uk_pollq *upq = (struct uk_pollq *)tick->arg;

		(void)ukarch_or(&ent->revents, set);
		uk_pollq_set_n(upq, UKFD_POLLIN,
			       IS_EDGEPOLL(ent) ? 1 : UK_POLLQ_NOTIFY_ALL);
		if (IS_ONESHOT(ent))
			tick->mask = 0;
	}
}



/* vfscore shim helpers */

static int vfs_poll(struct vfscore_file *vfd, unsigned int *revents,
		    struct eventpoll_cb *ecb)
{
	struct vnode *vnode = vfd->f_dentry->d_vnode;

	UK_ASSERT(vnode->v_op->vop_poll);
	return VOP_POLL(vnode, revents, ecb);
}

static void vfs_poll_register(struct vfscore_file *vfd,
			      struct epoll_legacy *leg)
{
	int ret;

	ret = vfs_poll(vfd, &leg->revents, &leg->ecb);
	if (unlikely(ret)) {
		leg->revents = EPOLLERR;
	} else {
		uk_list_add_tail(&leg->f_link, &vfd->f_ep);
		(void)ukarch_and(&leg->revents, leg->mask);
		if (leg->revents)
			uk_file_event_set(leg->epf, UKFD_POLLIN);
	}
}

/* File ops */

static void epoll_release(const struct uk_file *epf, int what)
{
	struct epoll_alloc *al = __containerof(epf, struct epoll_alloc, f);

	if (what & UK_FILE_RELEASE_RES) {
		struct epoll_entry **list = (struct epoll_entry **)epf->node;
		struct epoll_entry *p = *list;

		/* Free entries */
		while (p) {
			struct epoll_entry *ent = p;

			p = p->next;
			epoll_unregister_entry(ent);
			uk_free(al->alloc, ent);
		}
	}
	if (what & UK_FILE_RELEASE_OBJ) {
		/* Free alloc */
		uk_free(al->alloc, al);
	}
}

/* CTL ops */

static int epoll_add(const struct uk_file *epf, int fd, const struct uk_file *f,
		     const struct epoll_event *event)
{
	const int edge = !!(event->events & EPOLLET);
	int ret = 0;
	struct epoll_alloc *al = __containerof(epf, struct epoll_alloc, f);
	struct epoll_entry **tail = (struct epoll_entry **)epf->node;
	struct epoll_entry *ent;
	uk_pollevent ev;

	/* Look through list to make sure fd not present */
	while (*tail) {
		if (unlikely((*tail)->fd == fd)) {
			ret = -EEXIST;
			goto out;
		}
		tail = &(*tail)->next;
	}
	/* New entry */
	ent = uk_malloc(al->alloc, sizeof(*ent));
	if (unlikely(!ent)) {
		ret = -ENOMEM;
		goto out;
	}
	uk_file_acquire_weak(f);
	*ent = (struct epoll_entry){
		.next = NULL,
		.legacy = 0,
		.fd = fd,
		.f = f,
		.event = *event,
		.tick = UK_POLL_CHAIN_CALLBACK(events2mask(event->events),
					       epoll_event_callback,
					       &epf->state->pollq),
		.revents = 0
	};
	*tail = ent;
	/* Poll, register & update if needed */
	ev = uk_pollq_poll_register(&f->state->pollq, &ent->tick, 1);
	if (ev) {
		/* Need atomic OR since we're registered for updates */
		(void)ukarch_or(&ent->revents, ev);
		uk_pollq_set_n(&epf->state->pollq, UKFD_POLLIN,
			       edge ? 1 : UK_POLLQ_NOTIFY_ALL);
	}
out:
	return ret;
}

static int epoll_add_legacy(const struct uk_file *epf, int fd,
			    struct vfscore_file *vf,
			    const struct epoll_event *event)
{
	int ret = 0;
	struct epoll_alloc *al = __containerof(epf, struct epoll_alloc, f);
	struct epoll_entry **tail = (struct epoll_entry **)epf->node;
	struct epoll_entry *ent;

	/* Look through list to make sure fd not present */
	while (*tail) {
		if (unlikely((*tail)->fd == fd)) {
			UK_ASSERT((*tail)->legacy);
			ret = -EEXIST;
			goto out;
		}
		tail = &(*tail)->next;
	}
	/* New entry */
	ent = uk_malloc(al->alloc, sizeof(*ent));
	if (unlikely(!ent)) {
		ret = -ENOMEM;
		goto out;
	}
	*ent = (struct epoll_entry){
		.next = NULL,
		.legacy = 1,
		.fd = fd,
		.vf = vf,
		.event = *event,
		.legacy_cb = {
			.ecb = { .unregister = NULL, .data = NULL },
			.epf = epf,
			.mask = events2mask(event->events),
			.revents = 0
		}
	};
	UK_INIT_LIST_HEAD(&ent->legacy_cb.ecb.cb_link);
	UK_INIT_LIST_HEAD(&ent->legacy_cb.f_link);
	*tail = ent;
	/* Poll, register & update if needed */
	vfs_poll_register(vf, &ent->legacy_cb);
out:
	return ret;
}

static void entry_mod(struct epoll_entry *ent, const struct epoll_event *event)
{
	struct uk_poll_chain ntick;

	UK_ASSERT(!ent->legacy);
	ntick = ent->tick;
	ntick.mask = events2mask(event->events);

	uk_pollq_reregister(&ent->f->state->pollq, &ent->tick, &ntick);

	ent->event = *event;
	ent->revents = 0;
}

static void entry_mod_legacy(struct epoll_entry *ent,
			     const struct epoll_event *event)
{
	UK_ASSERT(ent->legacy);
	ent->legacy_cb.revents = 0;
	ent->legacy_cb.mask = events2mask(event->events);
	ent->event = *event;
	vfs_poll_register(ent->vf, &ent->legacy_cb);
}

static int epoll_mod(const struct uk_file *epf, int fd,
		     const struct epoll_event *event)
{
	int ret = -ENOENT;
	struct epoll_entry **p = (struct epoll_entry **)epf->node;

	while (*p) {
		struct epoll_entry *ent = *p;

		if (ent->fd == fd) {
			if (ent->legacy)
				entry_mod_legacy(ent, event);
			else
				entry_mod(ent, event);
			ret = 0;
			break;
		}
		p = &(*p)->next;
	}
	return ret;
}

static int epoll_del(const struct uk_file *epf, int fd)
{
	int ret = -ENOENT;
	struct epoll_alloc *al = __containerof(epf, struct epoll_alloc, f);
	struct epoll_entry **p = (struct epoll_entry **)epf->node;

	while (*p) {
		struct epoll_entry *ent = *p;

		if (fd == ent->fd) {
			*p = ent->next;
			epoll_unregister_entry(ent);
			uk_free(al->alloc, ent);
			ret = 0;
			break;
		}
		p = &(*p)->next;
	}
	return ret;
}

/* vfscore shim callbacks */

/* Called by vfscore drivers to signal events to epoll */
void eventpoll_signal(struct eventpoll_cb *ecb, unsigned int revents)
{
	struct epoll_legacy *leg = __containerof(ecb, struct epoll_legacy, ecb);

	revents &= leg->mask;
	if (revents) {
		(void)ukarch_or(&leg->revents, revents);
		uk_file_event_set(leg->epf, UKFD_POLLIN);
	}
}

/* Called by vfscore on monitored file close */
void eventpoll_notify_close(struct vfscore_file *fp)
{
	struct uk_list_head *itr;
	struct uk_list_head *tmp;

	uk_list_for_each_safe(itr, tmp, &fp->f_ep) {
		struct epoll_legacy *leg = uk_list_entry(
			itr, struct epoll_legacy, f_link);
		struct epoll_entry *ent = __containerof(
			leg, struct epoll_entry, legacy_cb);

		UK_ASSERT(ent->legacy);
		epoll_unregister_entry(ent);
		epoll_del(leg->epf, ent->fd);
	}
}

/* File creation */

struct uk_file *uk_epollfile_create(void)
{
	/* Alloc */
	struct uk_alloc *a = uk_alloc_get_default();
	struct epoll_alloc *al = uk_malloc(a, sizeof(*al));

	if (!al)
		return NULL;
	/* Set fields */
	al->alloc = a;
	al->list = NULL;
	al->fstate = UK_FILE_STATE_INITIALIZER(al->fstate);
	al->frefcnt = UK_FILE_REFCNT_INITIALIZER;
	al->f = (struct uk_file){
		.vol = EPOLL_VOLID,
		.node = &al->list,
		.refcnt = &al->frefcnt,
		.state = &al->fstate,
		.ops = &uk_file_nops,
		._release = epoll_release
	};
	/* ret */
	return &al->f;
}

/* Internal Syscalls */

int uk_sys_epoll_create(int flags)
{
	struct uk_file *f;
	unsigned int mode;
	int ret;

	if (unlikely(flags & ~EPOLL_CLOEXEC))
		return -EINVAL;

	f = uk_epollfile_create();
	if (unlikely(!f))
		return -ENOMEM;

	mode = O_RDONLY|UKFD_O_NOSEEK;
	if (flags & EPOLL_CLOEXEC)
		mode |= O_CLOEXEC;

	ret = uk_fdtab_open(f, mode);
	uk_file_release(f);
	return ret;
}

int uk_sys_epoll_ctl(const struct uk_file *epf, int op, int fd,
		     const struct epoll_event *event)
{
	int ret;
	union uk_shim_file sf;
	int legacy;

	if (unlikely(epf->vol != EPOLL_VOLID))
		return -EINVAL;

	legacy = uk_fdtab_shim_get(fd, &sf);
	if (unlikely(legacy < 0))
		return -EBADF;
	legacy = legacy == UK_SHIM_LEGACY;

	uk_file_wlock(epf);
	switch (op) {
	case EPOLL_CTL_ADD:
		if (legacy)
			ret = epoll_add_legacy(epf, fd, sf.vfile, event);
		else
			ret = epoll_add(epf, fd, sf.ofile->file, event);
		break;
	case EPOLL_CTL_MOD:
		ret = epoll_mod(epf, fd, event);
		break;
	case EPOLL_CTL_DEL:
		ret = epoll_del(epf, fd);
		break;
	default:
		ret = -EINVAL;
	}
	uk_file_wunlock(epf);

	if (legacy)
		fdrop(sf.vfile);
	else
		uk_fdtab_ret(sf.ofile);

	return ret;
}

int uk_sys_epoll_pwait2(const struct uk_file *epf, struct epoll_event *events,
			int maxevents, const struct timespec *timeout,
			const sigset_t *sigmask, size_t sigsetsize __unused)
{
	struct epoll_entry **list;
	__nsec deadline;

	if (unlikely(epf->vol != EPOLL_VOLID))
		return -EINVAL;
	if (unlikely(!events))
		return -EFAULT;
	if (unlikely(maxevents <= 0))
		return -EINVAL;
	if (unlikely(sigmask)) {
		uk_pr_warn_once("STUB: epoll_pwait no sigmask support\n");
		return -ENOSYS;
	}

	list = (struct epoll_entry **)epf->node;

	if (timeout) {
		__snsec tout = uk_time_spec_to_nsec(timeout);

		if (tout < 0)
			return -EINVAL;
		deadline = ukplat_monotonic_clock() + tout;
	} else {
		deadline = 0;
	}

	while (uk_file_poll_until(epf, UKFD_POLLIN, deadline)) {
		int lvlev = 0;
		int nout = 0;

		uk_file_event_clear(epf, UKFD_POLLIN);
		uk_file_rlock(epf);

		/* gather & output event list */
		for (struct epoll_entry *p = *list;
		     p && nout < maxevents;
		     p = p->next) {
			unsigned int revents;
			unsigned int *revp;

			if (p->legacy)
				revp = &p->legacy_cb.revents;
			else
				revp = &p->revents;

			revents = ukarch_exchange_n(revp, 0);
			if (revents) {
				if (!IS_EDGEPOLL(p)) {
					unsigned int mask;

					mask = events2mask(p->event.events);
					if (p->legacy) {
						vfs_poll(p->vf, &revents,
							 &p->legacy_cb.ecb);
						revents &= mask;
					} else {
						revents = uk_file_poll_immediate(p->f, mask);
					}
					if (!revents)
						continue;

					lvlev = 1;
					(void)ukarch_or(revp, revents);
				}

				events[nout].events = revents;
				events[nout].data = p->event.data;
				nout++;
			}
		}
		uk_file_runlock(epf);

		/* If lvlev, update pollin back in */
		if (lvlev)
			uk_file_event_set(epf, UKFD_POLLIN);

		if (nout)
			return nout;
		/* If nout == 0 loop back around */
	}
	/* Timeout */
	return 0;
}

/* Userspace Syscalls */

UK_SYSCALL_R_DEFINE(int, epoll_create, int, size)
{
	if (unlikely(size <= 0))
		return -EINVAL;
	return uk_sys_epoll_create(0);
}

UK_SYSCALL_R_DEFINE(int, epoll_create1, int, flags)
{
	return uk_sys_epoll_create(flags);
}

UK_SYSCALL_R_DEFINE(int, epoll_ctl, int, epfd, int, op, int, fd,
		    struct epoll_event *, event)
{
	int r;
	struct uk_ofile *of = uk_fdtab_get(epfd);

	if (unlikely(!of))
		return -EBADF;
	r = uk_sys_epoll_ctl(of->file, op, fd, event);
	uk_fdtab_ret(of);
	return r;
}

UK_SYSCALL_R_DEFINE(int, epoll_pwait2, int, epfd, struct epoll_event *, events,
		    int, maxevents, struct timespec *, timeout,
		    const sigset_t *, sigmask, size_t, sigsetsize)
{
	int r;
	struct uk_ofile *of = uk_fdtab_get(epfd);

	if (unlikely(!of))
		return -EBADF;
	r = uk_sys_epoll_pwait2(of->file, events, maxevents,
				timeout, sigmask, sigsetsize);
	uk_fdtab_ret(of);
	return r;
}

#ifdef epoll_pwait
#undef epoll_pwait
#endif

UK_LLSYSCALL_R_DEFINE(int, epoll_pwait, int, epfd, struct epoll_event *, events,
		      int, maxevents, int, timeout,
		      const sigset_t *, sigmask, size_t, sigsetsize)
{
	int r;
	struct uk_ofile *of = uk_fdtab_get(epfd);

	if (unlikely(!of))
		return -EBADF;
	r = uk_sys_epoll_pwait(of->file, events, maxevents,
			       timeout, sigmask, sigsetsize);
	uk_fdtab_ret(of);
	return r;
}

UK_SYSCALL_R_DEFINE(int, epoll_wait, int, epfd, struct epoll_event *, events,
		    int, maxevents, int, timeout)
{
	int r;
	struct uk_ofile *of = uk_fdtab_get(epfd);

	if (unlikely(!of))
		return -EBADF;
	r = uk_sys_epoll_pwait(of->file, events, maxevents,
			       timeout, NULL, 0);
	uk_fdtab_ret(of);
	return r;
}
