#include <inttypes.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <time.h>
#include <syslog.h>
#include <sys/types.h>
#include <sys/time.h>

#include "sm.h"
#include "sm_msg.h"
#include "disk_paxos.h"
#include "token_manager.h"
#include "lockfile.h"
#include "log.h"

struct token *tokens[MAX_LEASES];
pthread_t lease_threads[MAX_LEASES];
pthread_mutex_t lease_status_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t lease_status_cond = PTHREAD_COND_INITIALIZER;
struct lease_status lease_status[MAX_LEASES];
time_t _oldest_renewal_time; /* timestamp of oldest lease renewal */
int _stopping_all_leases;
struct sm_timeouts to;
int token_id_counter = 1;

/* return < 0 on error, 1 on success */

int acquire_lease(struct token *token, struct leader_record *leader)
{
	struct leader_record leader_ret;
	int rv;

	rv = disk_paxos_acquire(token, 0, &leader_ret);
	if (rv < 0)
		return rv;

	memcpy(leader, &leader_ret, sizeof(struct leader_record));
	return 1;
}

/* return < 0 on error, 1 on success */

int renew_lease(struct token *token, struct leader_record *leader)
{
	struct leader_record leader_ret;
	int rv;

	rv = disk_paxos_renew(token, leader, &leader_ret);
	if (rv < 0)
		return rv;

	memcpy(leader, &leader_ret, sizeof(struct leader_record));
	return 1;
}

/* return < 0 on error, 1 on success */

int release_lease(struct token *token, struct leader_record *leader)
{
	struct leader_record leader_ret;
	int rv;

	rv = disk_paxos_release(token, leader, &leader_ret);
	if (rv < 0)
		return rv;

	memcpy(leader, &leader_ret, sizeof(struct leader_record));
	return 1;
}
void set_lease_status(int index, int op, int r, uint64_t t)
{
	pthread_mutex_lock(&lease_status_mutex);
	switch (op) {
	case OP_ACQUIRE:
		lease_status[index].acquire_last_result = r;
		lease_status[index].acquire_last_time = t;
		if (r == DP_OK)
			lease_status[index].acquire_good_time = t;
		/* fall through, acquire works as renewal */

	case OP_RENEWAL:
		lease_status[index].renewal_last_result = r;
		lease_status[index].renewal_last_time = t;
		if (r == DP_OK)
			lease_status[index].renewal_good_time = t;
		break;

	case OP_RELEASE:
		lease_status[index].release_last_result = r;
		lease_status[index].release_last_time = t;
		if (r == DP_OK)
			lease_status[index].release_good_time = t;
		break;
	default:
		log_error(NULL, "invalid op %d", op);
	};
	pthread_cond_broadcast(&lease_status_cond);
	pthread_mutex_unlock(&lease_status_mutex);
}

uint64_t get_oldest_renewal_time(void)
{
	return _oldest_renewal_time;
}

void get_lease_result(int index, int op, int *r)
{
	pthread_mutex_lock(&lease_status_mutex);
	if (op == OP_ACQUIRE) {
		*r = lease_status[index].acquire_last_result;
	} else if (op == OP_RENEWAL) {
		*r = lease_status[index].renewal_last_result;
	} else if (op == OP_RELEASE) {
		*r = lease_status[index].release_last_result;
	}
	pthread_mutex_unlock(&lease_status_mutex);
}

int get_lease_status(int index, struct lease_status *status)
{
	//TODO : check if token id exists
	pthread_mutex_lock(&lease_status_mutex);
	memcpy(status, &lease_status[index], sizeof(struct lease_status));
	pthread_mutex_unlock(&lease_status_mutex);
	return 1;

}

int check_leases_renewed(void)
{
	uint64_t sec, oldest = 0;
	int fail_count = 0;
	int i;

	pthread_mutex_lock(&lease_status_mutex);
	for (i = 0; i < MAX_LEASES; i++) {
		if (!lease_status[i].thread_running)
			continue;

		if (lease_status[i].stop_thread)
			continue;

		/* this lease has not been acquired */
		if (!lease_status[i].renewal_good_time)
			continue;

		if (!oldest || (oldest < lease_status[i].renewal_good_time))
			oldest = lease_status[i].renewal_good_time;

		sec = time(NULL) - lease_status[i].renewal_good_time;

		if (sec >= to.lease_renewal_fail_seconds) {
			fail_count++;
			log_error(tokens[i], "renewal fail last result %d "
				  "at %llu good %llu",
				  lease_status[i].renewal_last_result,
				  (unsigned long long)lease_status[i].renewal_last_time,
				  (unsigned long long)lease_status[i].renewal_good_time);
		} else if (sec >= to.lease_renewal_warn_seconds) {
			log_error(tokens[i], "renewal delay last result %d "
				  "at %llu good %llu",
				  lease_status[i].renewal_last_result,
				  (unsigned long long)lease_status[i].renewal_last_time,
				  (unsigned long long)lease_status[i].renewal_good_time);
		}
	}
	pthread_mutex_unlock(&lease_status_mutex);

	_oldest_renewal_time = oldest;

	if (fail_count)
		return -1;

	return 0;
}

int stopping_all_leases(void)
{
	return _stopping_all_leases;
}

/* tell all threads to release and exit */

void stop_all_leases(void)
{
	int i;

	_stopping_all_leases = 1;

	pthread_mutex_lock(&lease_status_mutex);
	for (i = 0; i < MAX_LEASES; i++) {
		if (lease_status[i].thread_running)
			lease_status[i].stop_thread = 1;
	}
	pthread_cond_broadcast(&lease_status_cond);
	pthread_mutex_unlock(&lease_status_mutex);
}

/* This assumes that stop_all_leases() has been called, so all threads
   are stopping and it doesn't need to check any lease_status[] values.
   It's also ok to block the main thread doing this since there's nothing
   for it to continue monitoring. */

void cleanup_all_leases(void)
{
	struct token *token;
	int i;
	void *ret;

	/* sanity check */
	if (!_stopping_all_leases)
		log_error(NULL, "cleanup_all_leases before stop");

	for (i = 0; i < MAX_LEASES; i++) {
		token = tokens[i];
		if (token) {
			log_debug(token, "clean thread %d", i);
			pthread_join(lease_threads[i], &ret);
			free(token->disks);
			free(token);
			tokens[i] = NULL;
		}
	}
}

/* This is intended to clean up individual threads that have been stopped
   (e.g. sync_manager acquire failed or sync_manager release called) without
   all threads having been stopped (which cleanup_all_leases is for). */

void cleanup_stopped_lease(void)
{
	struct token *token;
	int found = 0;
	int i;
	void *ret;

	pthread_mutex_lock(&lease_status_mutex);
	for (i = 0; i < MAX_LEASES; i++) {
		if (lease_status[i].stop_thread &&
		    !lease_status[i].thread_running) {
			memset(&lease_status[i], 0, sizeof(struct lease_status));
			found = 1;
			break;
		}
	}
	pthread_mutex_unlock(&lease_status_mutex);

	if (!found)
		return;

	token = tokens[i];
	log_debug(token, "clean thread %d", i);
	pthread_join(lease_threads[i], &ret);
	free(token->disks);
	free(token);
	tokens[i] = NULL;
}

void set_thread_running(int index, int val)
{
	pthread_mutex_lock(&lease_status_mutex);
	lease_status[index].thread_running = val;

	/* stop_thread may not be 1 in the case where lease_thread
	   fails to do the initial acquire.  stop_thread needs to be 1
	   for clean_stopped_threads to clean it up. */
	if (!val)
		lease_status[index].stop_thread = 1;

	pthread_mutex_unlock(&lease_status_mutex);
}

void *lease_thread(void *arg)
{
	struct token *token = (struct token *)arg;
	struct leader_record leader;
	struct timespec ts;
	int index = token->index;
	int fd, rv, stop, num_opened;

	fd = lockfile(token, RESOURCE_LOCKFILE_DIR, token->resource_name);
	if (fd < 0) {
		set_lease_status(index, OP_ACQUIRE, -EBADF, 0);
		goto out_run;
	}

	num_opened = open_disks(token);
	if (!majority_disks(token, num_opened)) {
		log_error(token, "cannot open majority of disks");
		set_lease_status(index, OP_ACQUIRE, -ENODEV, 0);
		goto out_lockfile;
	}

	log_debug(token, "lease_thread index %d token_id %d acquire_lease...",
		  index, token->token_id);

	rv = acquire_lease(token, &leader);
	set_lease_status(index, OP_ACQUIRE, rv, leader.timestamp);
	if (rv < 0) {
		log_error(token, "acquire failed %d", rv);
		goto out_disks;
	}
	log_debug(token, "acquire at %llu",
		  (unsigned long long)leader.timestamp);

	while (1) {
#if 0
		sleep(to.lease_renewal_seconds);
		pthread_mutex_lock(&lease_status_mutex);
		stop = lease_status[index].stop_thread;
		pthread_mutex_unlock(&lease_status_mutex);
		if (stop)
			break;
#endif

		pthread_mutex_lock(&lease_status_mutex);
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_sec += to.lease_renewal_seconds;
		rv = 0;
		while (!lease_status[index].stop_thread && rv == 0) {
			rv = pthread_cond_timedwait(&lease_status_cond,
						    &lease_status_mutex, &ts);
		}
		stop = lease_status[index].stop_thread;
		pthread_mutex_unlock(&lease_status_mutex);
		if (stop)
			break;

		rv = renew_lease(token, &leader);
		set_lease_status(index, OP_RENEWAL, rv, leader.timestamp);
		if (rv < 0)
			log_error(token, "renewal failed %d", rv);
		else
			log_debug(token, "renewal");
	}

	rv = release_lease(token, &leader);
	set_lease_status(index, OP_RELEASE, rv, leader.timestamp);
	log_debug(token, "release rv %d", rv);

 out_disks:
	close_disks(token);
 out_lockfile:
	unlink_lockfile(fd, RESOURCE_LOCKFILE_DIR, token->resource_name);
 out_run:
	set_thread_running(index, 0);
	return NULL;
}

int create_token(int num_disks, struct token **token_out)
{
	struct token *token;
	struct paxos_disk *disks;

	token = malloc(sizeof(struct token));
	if (!token)
		return -ENOMEM;
	memset(token, 0, sizeof(struct token));

	disks = malloc(num_disks * sizeof(struct paxos_disk));
	if (!disks) {
		free(token);
		return -ENOMEM;
	}

	token->disks = disks;
	token->num_disks = num_disks;
	*token_out = token;
	return 0;
}

int add_lease_thread(struct token *token, int *id_ret)
{
	pthread_attr_t attr;
	int i, rv, id, found = 0;

	/* find an unused lease id, only main loop accesses
	   tokens[] and lease_threads[], no locking needed */

	for (i = 0; i < MAX_LEASES; i++) {
		if (!tokens[i]) {
			found = 1;
			break;
		}
	}
	if (!found) {
		log_error(token, "add lease failed, max leases in use");
		rv = -ENOSPC;
		goto out;
	}

	id = i;
	token->index = i;
	token->token_id = token_id_counter++;

	/* verify that the token id slot is unused in lease_status[],
	   and that that the resource_name is not already used */

	pthread_mutex_lock(&lease_status_mutex);
	for (i = 0; i < MAX_LEASES; i++) {
		if (!lease_status[i].thread_running)
			continue;
		if (strncmp(lease_status[i].resource_name, token->resource_name, NAME_ID_SIZE))
			continue;
		pthread_mutex_unlock(&lease_status_mutex);
		rv = -EINVAL;
		goto out;
	}
	if (lease_status[id].thread_running) {
		pthread_mutex_unlock(&lease_status_mutex);
		log_error(token, "add lease failed, thread %d running", id);
		rv = -EINVAL;
		goto out;
	}
	strncpy(lease_status[id].resource_name, token->resource_name, NAME_ID_SIZE);

	/* Changed here so that the initial state change will occur
	 * in the synchronous state. Otherwise there is a point
	 * where a thread is active but is not marked as such. */
	lease_status[id].thread_running = 1;
	lease_status[id].stop_thread = 0;

	pthread_mutex_unlock(&lease_status_mutex);

	pthread_attr_init(&attr);
	rv = pthread_create(&lease_threads[id], &attr, lease_thread, token);
	pthread_attr_destroy(&attr);
 out:
	if (rv < 0) {
		log_error(token, "add lease failed, thread create %d", rv);
		free(token->disks);
		free(token);
	} else {
		tokens[id] = token;
		*id_ret = id;
	}
	return rv;
}

int waitForStateCond(struct timespec timeout)
{
    int rv;
    pthread_mutex_lock(&lease_status_mutex);
    if ( (timeout.tv_sec == 0) && (timeout.tv_sec == 0) ) {
        rv = pthread_cond_wait(&lease_status_cond, &lease_status_mutex);
    } else {
        rv = pthread_cond_timedwait(&lease_status_cond, &lease_status_mutex, &timeout);
    }
    pthread_mutex_unlock(&lease_status_mutex);
    return rv;
}

int stop_token(int token_id) {
	pthread_mutex_lock(&lease_status_mutex);
	lease_status[token_id].stop_thread = 1;
	pthread_mutex_unlock(&lease_status_mutex);
	return 1;
}

int stop_lease(char *resource_name)
{
	int i, found = 0;

	pthread_mutex_lock(&lease_status_mutex);
	for (i = 0; i < MAX_LEASES; i++) {
		if (!lease_status[i].thread_running) {
			/* an old, stopped but not cleaned token */
			continue;
		}

		if (strncmp(lease_status[i].resource_name, resource_name, NAME_ID_SIZE))
			continue;
		lease_status[i].stop_thread = 1;
		found = 1;
		break;
	}
	pthread_mutex_unlock(&lease_status_mutex);

	if (found)
		return 0;
	return -ENOENT;
}

