#include <sys/mman.h>
#include <unistd.h>
#include <stdlib.h>

#include "pstree.h"
#include "restorer.h"
#include "util.h"

#include "protobuf.h"
#include "protobuf/pstree.pb-c.h"

struct pstree_item *root_item;

void free_pstree(struct pstree_item *root_item)
{
	struct pstree_item *item = root_item, *parent;

	while (item) {
		if (!list_empty(&item->children)) {
			item = list_first_entry(&item->children, struct pstree_item, sibling);
			continue;
		}

		parent = item->parent;
		list_del(&item->sibling);
		xfree(item->threads);
		xfree(item);
		item = parent;
	}
}

struct pstree_item *__alloc_pstree_item(bool rst)
{
	struct pstree_item *item;

	item = xzalloc(sizeof(*item) + (rst ? sizeof(item->rst[0]) : 0));
	if (!item)
		return NULL;

	INIT_LIST_HEAD(&item->children);
	INIT_LIST_HEAD(&item->sibling);

	item->pid.virt = -1;
	item->pid.real = -1;
	item->born_sid = -1;

	return item;
}

/* Deep first search on children */
struct pstree_item *pstree_item_next(struct pstree_item *item)
{
	if (!list_empty(&item->children))
		return list_first_entry(&item->children, struct pstree_item, sibling);

	while (item->parent) {
		if (item->sibling.next != &item->parent->children)
			return list_entry(item->sibling.next, struct pstree_item, sibling);
		item = item->parent;
	}

	return NULL;
}

int dump_pstree(struct pstree_item *root_item)
{
	struct pstree_item *item = root_item;
	PstreeEntry e = PSTREE_ENTRY__INIT;
	int ret = -1, i;
	int pstree_fd;

	pr_info("\n");
	pr_info("Dumping pstree (pid: %d)\n", root_item->pid.real);
	pr_info("----------------------------------------\n");

	/*
	 * Make sure we're dumping session leader, if not an
	 * appropriate option must be passed.
	 *
	 * Also note that if we're not a session leader we
	 * can't get the situation where the leader sits somewhere
	 * deeper in process tree, thus top-level checking for
	 * leader is enough.
	 */
	if (root_item->pid.virt != root_item->sid) {
		if (!opts.shell_job) {
			pr_err("The root process %d is not a session leader,"
			       "miss option?\n", item->pid.virt);
			return -1;
		}
	}

	pstree_fd = open_image(CR_FD_PSTREE, O_DUMP);
	if (pstree_fd < 0)
		return -1;

	for_each_pstree_item(item) {
		pr_info("Process: %d(%d)\n", item->pid.virt, item->pid.real);

		e.pid		= item->pid.virt;
		e.ppid		= item->parent ? item->parent->pid.virt : 0;
		e.pgid		= item->pgid;
		e.sid		= item->sid;
		e.n_threads	= item->nr_threads;

		e.threads = xmalloc(sizeof(e.threads[0]) * e.n_threads);
		if (!e.threads)
			goto err;

		for (i = 0; i < item->nr_threads; i++)
			e.threads[i] = item->threads[i].virt;

		ret = pb_write_one(pstree_fd, &e, PB_PSTREE);
		xfree(e.threads);

		if (ret)
			goto err;
	}
	ret = 0;

err:
	pr_info("----------------------------------------\n");
	close(pstree_fd);
	return ret;
}

static int max_pid = 0;

static int prepare_pstree_for_shell_job(struct pstree_item *root)
{
	pid_t current_sid = getsid(getpid());
	pid_t current_gid = getpgid(getpid());

	struct pstree_item *pi;

	pid_t old_sid;
	pid_t old_gid;

	if (!opts.shell_job)
		return 0;

	/*
	 * Migration of a root task group leader is a bit tricky.
	 * When a task yields SIGSTOP, the kernel notifies the parent
	 * with SIGCHLD. This means when task is running in a
	 * shell, the shell obtains SIGCHLD and sends a task to
	 * the background.
	 *
	 * The situation gets changed once we restore the
	 * program -- our tool become an additional stub between
	 * the restored program and the shell. So to be able to
	 * notify the shell with SIGCHLD from our restored
	 * program -- we make the root task to inherit the
	 * process group from us.
	 *
	 * Not that clever solution but at least it works.
	 */

	old_sid = root->sid;
	old_gid = root->pgid;

	pr_info("Migrating process tree (GID %d->%d SID %d->%d)\n",
		old_gid, current_gid, old_sid, current_sid);

	for_each_pstree_item(pi) {
		if (pi->pgid == old_gid)
			pi->pgid = current_gid;
		if (pi->sid == old_sid)
			pi->sid = current_sid;
	}

	max_pid = max((int)current_sid, max_pid);
	max_pid = max((int)current_gid, max_pid);

	return 0;
}

int prepare_pstree(void)
{
	int ret = 0, i, ps_fd;
	struct pstree_item *pi, *parent = NULL;

	pr_info("Reading image tree\n");

	ps_fd = open_image_ro(CR_FD_PSTREE);
	if (ps_fd < 0)
		return ps_fd;

	while (1) {
		PstreeEntry *e;

		ret = pb_read_one_eof(ps_fd, &e, PB_PSTREE);
		if (ret <= 0)
			break;

		ret = -1;
		pi = alloc_pstree_item_with_rst();
		if (pi == NULL)
			break;

		pi->pid.virt = e->pid;
		max_pid = max((int)e->pid, max_pid);

		pi->pgid = e->pgid;
		max_pid = max((int)e->pgid, max_pid);

		pi->sid = e->sid;
		max_pid = max((int)e->sid, max_pid);

		if (e->ppid == 0) {
			if (root_item) {
				pr_err("Parent missed on non-root task "
				       "with pid %d, image corruption!\n", e->pid);
				goto err;
			}
			root_item = pi;
			pi->parent = NULL;
		} else {
			/*
			 * Fast path -- if the pstree image is not edited, the
			 * parent of any item should have already being restored
			 * and sit among the last item's ancestors.
			 */
			while (parent) {
				if (parent->pid.virt == e->ppid)
					break;
				parent = parent->parent;
			}

			if (parent == NULL) {
				for_each_pstree_item(parent) {
					if (parent->pid.virt == e->ppid)
						break;
				}

				if (parent == NULL) {
					pr_err("Can't find a parent for %d", pi->pid.virt);
					pstree_entry__free_unpacked(e, NULL);
					xfree(pi);
					goto err;
				}
			}

			pi->parent = parent;
			list_add(&pi->sibling, &parent->children);
		}

		parent = pi;

		pi->nr_threads = e->n_threads;
		pi->threads = xmalloc(e->n_threads * sizeof(struct pid));
		if (!pi->threads)
			break;

		ret = 0;
		for (i = 0; i < e->n_threads; i++)
			pi->threads[i].virt = e->threads[i];

		task_entries->nr += e->n_threads;
		task_entries->nr_tasks++;

		pstree_entry__free_unpacked(e, NULL);
	}

	ret = prepare_pstree_for_shell_job(root_item);
err:
	close(ps_fd);
	return ret;
}

int prepare_pstree_ids(void)
{
	struct pstree_item *item, *child, *helper, *tmp;
	LIST_HEAD(helpers);

	pid_t current_pgid = getpgid(getpid());

	/*
	 * Some task can be reparented to init. A helper task should be added
	 * for restoring sid of such tasks. The helper tasks will be exited
	 * immediately after forking children and all children will be
	 * reparented to init.
	 */
	list_for_each_entry(item, &root_item->children, sibling) {

		/*
		 * If a child belongs to the root task's session or it's
		 * a session leader himself -- this is a simple case, we
		 * just proceed in a normal way.
		 */
		if (item->sid == root_item->sid || item->sid == item->pid.virt)
			continue;

		helper = alloc_pstree_item();
		if (helper == NULL)
			return -1;
		helper->sid = item->sid;
		helper->pgid = item->sid;
		helper->pid.virt = item->sid;
		helper->state = TASK_HELPER;
		helper->parent = root_item;
		list_add_tail(&helper->sibling, &helpers);
		task_entries->nr_helpers++;

		pr_info("Add a helper %d for restoring SID %d\n",
				helper->pid.virt, helper->sid);

		child = list_entry(item->sibling.prev, struct pstree_item, sibling);
		item = child;

		/*
		 * Stack on helper task all children with target sid.
		 */
		list_for_each_entry_safe_continue(child, tmp, &root_item->children, sibling) {
			if (child->sid != helper->sid)
				continue;
			if (child->sid == child->pid.virt)
				continue;

			pr_info("Attach %d to the temporary task %d\n",
					child->pid.virt, helper->pid.virt);

			child->parent = helper;
			list_move(&child->sibling, &helper->children);
		}
	}

	/* Try to connect helpers to session leaders */
	for_each_pstree_item(item) {
		if (!item->parent) /* skip the root task */
			continue;

		if (item->state == TASK_HELPER)
			continue;

		if (item->sid != item->pid.virt) {
			struct pstree_item *parent;

			if (item->parent->sid == item->sid)
				continue;

			/* the task could fork a child before and after setsid() */
			parent = item->parent;
			while (parent && parent->pid.virt != item->sid) {
				if (parent->born_sid != -1 && parent->born_sid != item->sid) {
					pr_err("Can't determinate with which sid (%d or %d)"
						"the process %d was born\n",
						parent->born_sid, item->sid, parent->pid.virt);
					return -1;
				}
				parent->born_sid = item->sid;
				pr_info("%d was born with sid %d\n", parent->pid.virt, item->sid);
				parent = parent->parent;
			}

			if (parent == NULL) {
				pr_err("Can't find a session leader for %d\n", item->sid);
				return -1;
			}

			continue;
		}

		pr_info("Session leader %d\n", item->sid);

		/* Try to find helpers, who should be connected to the leader */
		list_for_each_entry(child, &helpers, sibling) {
			if (child->state != TASK_HELPER)
				continue;

			if (child->sid != item->sid)
				continue;

			child->pgid = item->pgid;
			child->pid.virt = ++max_pid;
			child->parent = item;
			list_move(&child->sibling, &item->children);

			pr_info("Attach %d to the task %d\n",
					child->pid.virt, item->pid.virt);

			break;
		}
	}

	/* All other helpers are session leaders for own sessions */
	list_splice(&helpers, &root_item->children);

	/* Add a process group leader if it is absent  */
	for_each_pstree_item(item) {
		struct pstree_item *gleader;

		if (!item->pgid || item->pid.virt == item->pgid)
			continue;

		for_each_pstree_item(gleader) {
			if (gleader->pid.virt == item->pgid)
				break;
		}

		if (gleader)
			continue;

		/*
		 * If the PGID is eq to current one -- this
		 * means we're inheriting group from the current
		 * task so we need to escape creating a helper here.
		 */
		if (current_pgid == item->pgid)
			continue;

		helper = alloc_pstree_item();
		if (helper == NULL)
			return -1;
		helper->sid = item->sid;
		helper->pgid = item->pgid;
		helper->pid.virt = item->pgid;
		helper->state = TASK_HELPER;
		helper->parent = item;
		list_add(&helper->sibling, &item->children);
		task_entries->nr_helpers++;

		pr_info("Add a helper %d for restoring PGID %d\n",
				helper->pid.virt, helper->pgid);
	}

	return 0;
}

bool restore_before_setsid(struct pstree_item *child)
{
	int csid = child->born_sid == -1 ? child->sid : child->born_sid;

	if (child->parent->born_sid == csid)
		return true;

	return false;
}
