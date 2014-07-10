/*
 * Copyright (C) 2014      Suse
 * Author Bamvor Jian Zhang <bjzhang@suse.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; version 2.1 only. with the special
 * exception on linking described in file LICENSE.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 */

#include "libxl_osdeps.h"

#include <glob.h>

#include "libxl_internal.h"

#define SNAPSHOT_ROOT "/var/lib/xen/snapshots"

static const char *snapshot_path(libxl__gc *gc, uint32_t domid);

static const char *snapshot_data_path(libxl__gc *gc, uint32_t domid,
                                      const char *name,
                                      const char *type,
                                      const char *wh);

static int libxl_snapshot_data_store(libxl_ctx *ctx, uint32_t domid,
                              libxl_domain_snapshot *snapshot,
                              const char *type, const uint8_t *data,
                              int datalen);

static int libxl_snapshot_data_retrieve(libxl_ctx *ctx, uint32_t domid,
                                 libxl_domain_snapshot *snapshot,
                                 const char *type,
                                 uint8_t **data_r, int *datalen_r);

static char **libxl__snapshot_data_listall(libxl__gc *gc, uint32_t domid, int *num);

int libxl_load_dom_snapshot_conf(libxl_ctx *ctx, uint32_t domid,
                                 libxl_domain_snapshot *snapshot)
{
    GC_INIT(ctx);
    uint8_t *data = 0;
    int rc, len;
    int i;

    rc = libxl_snapshot_data_retrieve(ctx, domid, snapshot, "libxl-json",
                                      &data, &len);
    if (rc) {
        LIBXL__LOG_ERRNOVAL(ctx, LIBXL__LOG_ERROR, rc,
                            "Failed to retrieve domain configuration for domain %d",
                            domid);
        rc = ERROR_FAIL;
        goto out;
    }

    if (len == 0) {
        LIBXL__LOG_ERRNO(ctx, LIBXL__LOG_ERROR,
                         "Configuration data stream empty for domain %d",
                         domid);
        rc = ERROR_FAIL;
        goto out;
    }

    /* Make sure this string ends with \0 -- the parser expects a NULL
     * terminated string.
     */
    if (data[len-1] != '\0') {
        data = libxl__realloc(gc, data, len + 1);
        data[len] = '\0';
    }

    rc = libxl_domain_snapshot_from_json(ctx, snapshot, (const char *)data);

    if ( strlen(snapshot->name) == 0 ) {
        rc = ERROR_INVAL;
        goto out;
    }
    if ( strlen(snapshot->description) == 0 ) {
        free(snapshot->description);
        snapshot->description = NULL;
    }
    if ( strlen(snapshot->memory) == 0 ) {
        free(snapshot->memory);
        snapshot->memory = NULL;
    }
    //do not store device path as it should be get from domain configuration.
    for ( i = 0; i < snapshot->num_disks; i++ ) {
        free(snapshot->disks[i].path);
        snapshot->disks[i].path = strdup("");
        if ( strlen(snapshot->disks[i].file) == 0 ) {
            free(snapshot->disks[i].file);
            snapshot->disks[i].file = NULL;
        }
    }

out:
    free(data);
    GC_FREE;
    return rc;
}

int libxl_store_dom_snapshot_conf(libxl_ctx *ctx, uint32_t domid,
                                  libxl_domain_snapshot *snapshot)
{
    GC_INIT(ctx);
    char *config_json;
    int rc;
    int i;

    if ( strlen(snapshot->name) == 0 ) {
        rc = ERROR_INVAL;
        goto out;
    }
    if ( !snapshot->description ) {
        snapshot->description = strdup("");
    }
    if ( !snapshot->memory ) {
        snapshot->memory = strdup("");
    }
    //do not store device path as it should be get from domain configuration.
    for ( i = 0; i < snapshot->num_disks; i++ ) {
        free(snapshot->disks[i].path);
        snapshot->disks[i].path = strdup("");
        if ( !snapshot->disks[i].file ) {
            snapshot->disks[i].file = strdup("");
        }
    }

    config_json = libxl_domain_snapshot_to_json(ctx, snapshot);

    rc = libxl_snapshot_data_store(ctx, domid, snapshot, "libxl-json",
                              (const uint8_t *)config_json,
                              strlen(config_json));
    if (rc) {
        LIBXL__LOG_ERRNOVAL(ctx, LIBXL__LOG_ERROR, rc,
                            "Failed to store domain snapshot configuration for domain %d<snapshot: %s>",
                            domid, snapshot->name);
        rc = ERROR_FAIL;
        goto out;
    }

    free(config_json);

out:
    GC_FREE;
    return rc;
}

int libxl_delete_dom_snapshot_conf(libxl_ctx *ctx, uint32_t domid,
                                   libxl_domain_snapshot *snapshot)
{
    const char *filename;
    int rc;

    GC_INIT(ctx);

    filename = snapshot_data_path(gc, domid, snapshot->name, "libxl-json", "d");
    if (!filename) {
        rc = ERROR_NOMEM;
        goto out;
    }

    rc = libxl__userdata_delete(gc, filename);
    if ( rc )
        rc = ERROR_FAIL;

out:
    GC_FREE;
    return rc;
}

libxl_domain_snapshot *libxl_list_dom_snapshot(libxl_ctx *ctx, uint32_t domid, int *num)
{
    int i, j = 0;
    char **list;
    libxl_domain_snapshot *snapshot = NULL;
    int e, rc;
    int datalen = 0;
    char *data = 0;

    GC_INIT(ctx);
    list = libxl__snapshot_data_listall(gc, domid, num);

    if ( !list )
        goto out;

    snapshot = malloc(sizeof(libxl_domain_snapshot) * (*num));
    if ( !snapshot ) {
        LIBXL__LOG(ctx, LIBXL__LOG_ERROR, "oom while allocate snapshot array, number: %d", *num);
        goto out;
    }

    for ( i = 0; i < *num; i++ ) {
        e = libxl_read_file_contents(ctx, list[i], (void **)&data, &datalen);
        if (e && errno != ENOENT) {
            continue;
        }
        if (!e && !datalen) {
            LIBXL__LOG(ctx, LIBXL__LOG_ERROR, "userdata file %s is empty", list[i]);
            continue;
        }

        /* Make sure this string ends with \0 -- the parser expects a NULL
         * terminated string.
         */
        if (data[datalen-1] != '\0') {
            data = libxl__realloc(gc, data, datalen + 1);
            data[datalen] = '\0';
        }

        rc = libxl_domain_snapshot_from_json(ctx, &snapshot[i], (const char *)data);
        free(data);
        data = NULL;
        j++;
    }
    if ( j == 0 ) {
        *num = 0;
        free(snapshot);
        snapshot = NULL;
    } else if ( *num != j ) {
        *num = j;
        snapshot = realloc(snapshot, sizeof(libxl_domain_snapshot) * (*num));
    }

out:
    GC_FREE;
    return snapshot;
}

int libxl_domain_snapshot_delete_save(libxl_ctx *ctx,
                                      libxl_domain_snapshot *snapshot)
{
    int rc;

    GC_INIT(ctx);
    rc = libxl__remove_file(gc, snapshot->memory);
    GC_FREE;

    return rc;
}

int libxl_disk_snapshot_create(libxl_ctx *ctx, uint32_t domid,
                               libxl_disk_snapshot *snapshot, int nb)
{
    int rc;
    GC_INIT(ctx);
    rc = libxl__qmp_disk_snapshot_transaction(gc, domid, snapshot, nb);
    if ( rc )
        LIBXL__LOG(ctx, LIBXL__LOG_ERROR, "domain disk snapshot create fail\n");

    GC_FREE;
    return rc;
}

int libxl_disk_snapshot_delete(libxl_ctx *ctx, uint32_t domid,
                               libxl_disk_snapshot *snapshot, int nb)
{
    int rc = 0;
    int i;

    GC_INIT(ctx);
    for(i = 0; i < nb; i++ ) {
        if ( snapshot[i].type == LIBXL_SNAPSHOT_TYPE_EXTERNAL )
            continue;

        rc = libxl__qmp_disk_snapshot_delete_internal(gc, domid, &snapshot[i]);
        if ( rc )
            goto err;
    }
err:
    if ( rc )
        LIBXL__LOG(ctx, LIBXL__LOG_ERROR, "domain disk snapshot delete fail\n");

    GC_FREE;
    return rc;
}

//if *snapshot is not NULL, *num should valid too.
int libxl_disk_to_snapshot(libxl_ctx *ctx, uint32_t domid, libxl_disk_snapshot **snapshot, int *num)
{
    int i, j, k, nb;
    int found;
    libxl_device_disk *disks;
    libxl_diskinfo diskinfo;
    char *path = NULL;
    bool new;
    bool snapshottable = false;
    int num_of_s_disk = 0; //number of snapshottable disk

    disks = libxl_device_disk_list(ctx, domid, &nb);
    if (!disks) {
        return -1;
    }

    if ( *snapshot ) {
        new = false;
    } else {
        *snapshot = (libxl_disk_snapshot*)malloc(sizeof(libxl_disk_snapshot) * nb);
        memset(*snapshot, 0, sizeof(libxl_disk_snapshot) * nb);
        new = true;
    }

    LIBXL__LOG(ctx, LIBXL__LOG_DEBUG, "fill disk information to disk snapshot");
    j = 0;
    num_of_s_disk = 0;
    for ( i = 0; i < nb; i++ ) {
        if (!libxl_device_disk_getinfo(ctx, domid, &disks[i], &diskinfo)) {
            LIBXL__LOG(ctx, LIBXL__LOG_DEBUG, "device<%s> info: frontend<%s>, "
                       "param<%s>", diskinfo.dev, diskinfo.frontend,
                       diskinfo.param);
            if ( !new ) {
                for ( k = 0; k < *num; k++ ) {
                    if ( !strcmp((*snapshot)[k].device, diskinfo.dev) ) {
                        break;
                    }
                }
                found = k;
            } else {
                found = j;
            }
            if ( strncmp(diskinfo.param, "qcow2:", strlen("qcow2:")) == 0 ) {
            //params = "qcow2:/var/lib/xen/images/opensuse12_3_01/disk0.qcow2"
                LIBXL__LOG(ctx, LIBXL__LOG_INFO, "disk<%s> support internal and external snasphot",
                           diskinfo.dev);
                snapshottable = true;
                (*snapshot)[found].format = LIBXL_DISK_FORMAT_QCOW2;
            } else if ( strncmp(diskinfo.param, "aio:", strlen("aio:")) == 0
                 && strncmp(diskinfo.type, "qdisk", strlen("qdisk") ) ) {
            //params = "aio:/var/lib/xen/images/bjz_04_sles11_sp2/disk0.raw"
                LIBXL__LOG(ctx, LIBXL__LOG_INFO, "disk<%s> support external snasphot",
                           diskinfo.dev);
                snapshottable = true;
                (*snapshot)[found].format = LIBXL_DISK_FORMAT_RAW;
                (*snapshot)[found].type = LIBXL_SNAPSHOT_TYPE_EXTERNAL;
            } else {
                snapshottable = false;
                if ( !new ) {
                    //TODO report error for config file.
                }
            }
            if ( snapshottable ) {
                if ( new )
                    (*snapshot)[found].device = strdup(diskinfo.dev);

                path = strchr(diskinfo.param, ':');
                path++;
                (*snapshot)[found].path = strdup(path);
                j++;
                num_of_s_disk++;
            }
            libxl_diskinfo_dispose(&diskinfo);
        }
        libxl_device_disk_dispose(&disks[i]);
    }
    free(disks);
    *snapshot = (libxl_disk_snapshot*)realloc(*snapshot, sizeof(libxl_disk_snapshot) * num_of_s_disk);
    *num = num_of_s_disk;
    LIBXL__LOG(ctx, LIBXL__LOG_INFO, "total %d number of disk could snapshot", num_of_s_disk);

    return 0;
}

const char *libxl_snapshot_data_path(libxl_ctx *ctx, uint32_t domid,
                                     libxl_domain_snapshot *snapshot,
                                     const char *type)
{
    const char *path;
    const char *path_r;

    GC_INIT(ctx);
    path = snapshot_data_path(gc, domid, (const char*)snapshot->name, type, "d");
    path_r = strdup(path);
    GC_FREE;
    return path_r;
}

char *libxl_snapshot_save_path(libxl_ctx *ctx, uint32_t domid, libxl_domain_snapshot *snapshot)
{
    const char *path;
    char *path_r;

    GC_INIT(ctx);
    path = GCSPRINTF("%s/%s.save", snapshot_path(gc , domid), snapshot->name);
    path_r = strdup(path);
    GC_FREE;
    return path_r;
}

int check_domain_snapshot_directory(libxl_ctx *ctx, uint32_t domid)
{
    const char *path;
    int rc, r;

    GC_INIT(ctx);

    for (;;) {
        r = mkdir(SNAPSHOT_ROOT, 0600);
        if (!r) break;
        if (errno == EINTR) continue;
        if (errno == EEXIST) break;
        LOGE(ERROR, "failed to create snapshot root dir %s", SNAPSHOT_ROOT);
        rc = ERROR_FAIL;
        goto out;
    }

    for (;;) {
        path = snapshot_path(gc, domid);
        r = mkdir(path, 0600);
        if (!r) break;
        if (errno == EINTR) continue;
        if (errno == EEXIST) break;
        LOGE(ERROR, "failed to create domain snapshot dir %s", path);
        rc = ERROR_FAIL;
        goto out;
    }

out:
    GC_FREE;
    return rc;
}

static const char *snapshot_path(libxl__gc *gc, uint32_t domid)
{
    libxl_ctx *ctx = libxl__gc_owner(gc);
    char *uuid_string;
    libxl_dominfo info;
    int rc;

    rc = libxl_domain_info(ctx, &info, domid);
    if (rc) {
        LOGE(ERROR, "unable to find domain info for domain %"PRIu32, domid);
        return NULL;
    }
    uuid_string = GCSPRINTF(LIBXL_UUID_FMT, LIBXL_UUID_BYTES(info.uuid));

    return GCSPRINTF("%s/%s", SNAPSHOT_ROOT, uuid_string);
}

//TODO: duplicated code with userdata_xxx
static const char *snapshot_data_path(libxl__gc *gc, uint32_t domid,
                                      const char *name,
                                      const char *type,
                                      const char *wh)
{
    libxl_ctx *ctx = libxl__gc_owner(gc);
    char *uuid_string;
    libxl_dominfo info;
    int rc;

    rc = libxl_domain_info(ctx, &info, domid);
    if (rc) {
        LOGE(ERROR, "unable to find domain info for domain %"PRIu32, domid);
        return NULL;
    }
    uuid_string = GCSPRINTF(LIBXL_UUID_FMT, LIBXL_UUID_BYTES(info.uuid));

    return GCSPRINTF("%s/snapshotdata-%s.%s.%s", snapshot_path(gc, domid),
                     wh, name, type);
}

static int libxl_snapshot_data_store(libxl_ctx *ctx, uint32_t domid,
                              libxl_domain_snapshot *snapshot,
                              const char *type, const uint8_t *data,
                              int datalen)
{
    GC_INIT(ctx);
    const char *filename;
    const char *newfilename;
    int e, rc;
    int fd = -1;

    filename = snapshot_data_path(gc, domid, snapshot->name, type, "d");
    if (!filename) {
        rc = ERROR_NOMEM;
        goto out;
    }

    if (!datalen) {
        rc = libxl__userdata_delete(gc, filename);
        goto out;
    }

    newfilename = snapshot_data_path(gc, domid, snapshot->name, type, "n");
    if (!newfilename) {
        rc = ERROR_NOMEM;
        goto out;
    }

    rc = ERROR_FAIL;

    fd = open(newfilename, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        goto err;

    if (libxl_write_exactly(ctx, fd, data, datalen, "snapshot_data", newfilename))
        goto err;

    if (close(fd) < 0) {
        fd = -1;
        goto err;
    }
    fd = -1;

    if (rename(newfilename, filename))
        goto err;

    rc = 0;

err:
    if (fd >= 0) {
        e = errno;
        close(fd);
        errno = e;
    }

    if (rc)
        LIBXL__LOG_ERRNO(ctx, LIBXL__LOG_ERROR, "cannot write/rename %s for %s",
                 newfilename, filename);
out:
    GC_FREE;
    return rc;
}

static int libxl_snapshot_data_retrieve(libxl_ctx *ctx, uint32_t domid,
                                 libxl_domain_snapshot *snapshot,
                                 const char *type,
                                 uint8_t **data_r, int *datalen_r)
{
    GC_INIT(ctx);
    const char *filename;
    int e, rc;
    int datalen = 0;
    void *data = 0;

    filename = snapshot_data_path(gc, domid, snapshot->name, type, "d");
    if (!filename) {
        rc = ERROR_NOMEM;
        goto out;
    }

    e = libxl_read_file_contents(ctx, filename, data_r ? &data : 0, &datalen);
    if (e && errno != ENOENT) {
        rc = ERROR_FAIL;
        goto out;
    }
    if (!e && !datalen) {
        LIBXL__LOG(ctx, LIBXL__LOG_ERROR, "userdata file %s is empty", filename);
        if (data_r) assert(!*data_r);
        rc = ERROR_FAIL;
        goto out;
    }

    if (data_r) *data_r = data;
    if (datalen_r) *datalen_r = datalen;
    rc = 0;
out:
    GC_FREE;
    return rc;
}

static char **libxl__snapshot_data_listall(libxl__gc *gc, uint32_t domid, int *num)
{
    const char *pattern;
    glob_t gl;
    int r, i;
    char **paths = NULL;

    pattern = snapshot_data_path(gc, domid, "*", "*", "?");
    if (!pattern)
        goto out;

    gl.gl_pathc = 0;
    gl.gl_pathv = 0;
    gl.gl_offs = 0;
    r = glob(pattern, GLOB_ERR|GLOB_NOSORT|GLOB_MARK, 0, &gl);
    if (r == GLOB_NOMATCH)
        goto out;
    if (r)
        LOGE(ERROR, "glob failed for %s", pattern);

    *num = gl.gl_pathc;
    paths = malloc(*num * sizeof(*paths));
    for (i=0; i<*num; i++) {
        paths[i] = strdup(gl.gl_pathv[i]);
    }
    globfree(&gl);
out:
    return paths;
}

static void snapshot_finished(libxl__egc *egc, libxl__ev_child *child,
                              pid_t pid, int status)
{
    libxl__ao_snapshot *ao_snapshot = CONTAINER_OF(child, libxl__ao_snapshot, child);
    STATE_AO_GC(ao_snapshot->ao);
    int rc = 0;

    if (status) {
        LOG(ERROR, "qemu-img failed");
        libxl_report_child_exitstatus(CTX, XTL_ERROR, "qemu-img",
                                      pid, status);
        rc = ERROR_FAIL;
        goto out;
    } else {
        LOG(DEBUG, "qemu-img completed");
    }

out:
    libxl__ao_complete(egc, ao, rc);
    return;
}

//FIXME: op in enum.
static int libxl_disk_snapshot_helper(libxl_ctx *ctx, uint32_t domid, libxl_disk_snapshot *snapshot,
                                      libxl_snapshot_op op, const libxl_asyncop_how *ao_how)
{
    AO_CREATE(ctx, domid, ao_how);
    libxl__ao_snapshot *ao_snapshot;
    const char **args;
    int num = 4 + 1;//1 for end NULL
    int i = 0;
    const char *operation;
    int rc;

    GCNEW(ao_snapshot);

    ao_snapshot->ao = ao;
    switch ( op ) {
        case LIBXL_SNAPSHOT_OP_REVERT:
            num++;
            operation = libxl__strdup(gc, "-a");
            break;
        case LIBXL_SNAPSHOT_OP_CREATE:
        case LIBXL_SNAPSHOT_OP_LIST:
        case LIBXL_SNAPSHOT_OP_DELETE:
            LOG(WARN, "only support revert right now");
            rc = ERROR_INVAL;
            break;
    }
    args = libxl__zalloc(gc, num * sizeof(*args));
    //FIXME: path.
    args[i++] = LIBEXEC "/" "qemu-img";
    args[i++] = libxl__strdup(gc, "snapshot");
    args[i++] = operation;
    args[i++] = libxl__strdup(gc, snapshot->name);
    args[i++] = libxl__strdup(gc, snapshot->path);

    pid_t pid = libxl__ev_child_fork(gc, &ao_snapshot->child, snapshot_finished);
    if ( pid == -1 ) {
        rc = ERROR_FAIL;
        goto out;
    }

    if ( !pid ) {
        /* child */
        struct stat st;
        while ( stat("/tmp/bamvor_dbg", &st) == 0 );
        libxl__exec(gc, -1, -1, -1, args[0], (char **)args, NULL);
        exit(-1);
    }

    /* parent */
    return AO_INPROGRESS;
out:
    return rc;
}

int libxl_disk_snapshot_revert(libxl_ctx *ctx, uint32_t domid, libxl_disk_snapshot *snapshot, int nb)
{
    int i;
    int rc = 0;

    for ( i = 0; i < nb; i++ ) {
        rc = libxl_disk_snapshot_helper(ctx, domid, &snapshot[i], LIBXL_SNAPSHOT_OP_REVERT, NULL);
    }
    return rc;
}

void libxl_set_disk_snapshot_name(libxl_domain_snapshot *snapshot)
{
    int i;

    for ( i = 0; i< snapshot->num_disks; i++ ) {
        if ( !snapshot->disks[i].name )
            snapshot->disks[i].name = strdup(snapshot->name);
    }
}

void libxl_set_disk_snapshot_type(libxl_domain_snapshot *snapshot)
{
    int i;

    for ( i = 0; i< snapshot->num_disks; i++ ) {
        if ( snapshot->disks[i].type == LIBXL_SNAPSHOT_TYPE_ANY )
            snapshot->disks[i].type = snapshot->type;
    }
}

