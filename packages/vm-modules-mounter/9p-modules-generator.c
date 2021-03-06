#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <glob.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>

static int mountpoint_to_unit_name(const char *mountpoint, char **ret)
{
	const char *p;
	const char *end;
	size_t path_len;
	char unit_name[NAME_MAX + 1];
	size_t i = 0;

	if (strcmp(mountpoint, "/") == 0) {
		strcpy(unit_name, "-");
		goto add_suffix;
	}

	path_len = strlen(mountpoint);
	assert(path_len > 0);
	end = &mountpoint[path_len];
	if (end[-1] == '/')
		end--;
	p = mountpoint;
	if (*p == '/')
		p++;
	assert(p != end);

	while (p != end) {
		if (*p == '/') {
			if (i >= NAME_MAX) {
				errno = ENAMETOOLONG;
				return -1;
			}
			unit_name[i++] = '-';
		} else if (isalnum(*p) || *p == '_' || (*p == '.' && i > 0)) {
			if (i >= NAME_MAX) {
				errno = ENAMETOOLONG;
				return -1;
			}
			unit_name[i++] = *p;
		} else {
			if (i >= NAME_MAX - 4) {
				errno = ENAMETOOLONG;
				return -1;
			}
			sprintf(&unit_name[i], "\\x%02x", (unsigned char)*p);
			i += 4;
		}
		p++;
	}
	unit_name[i++] = '\0';

add_suffix:
	if (asprintf(ret, "%s.mount", unit_name) == -1)
		return -1;
	return 0;
}

static int vfprintf_new_file(const char *path, const char *format, va_list ap)
{
	FILE *f = NULL;
	int ret;

	f = fopen(path, "w");
	if (f == NULL) {
		perror("fopen");
		return -1;
	}

	ret = vfprintf(f, format, ap);
	if (ret == -1)
		perror("vfprintf");

	if (fclose(f) == EOF) {
		perror("fclose");
		ret = -1;
	}
	return ret;
}

static int fprintf_new_file(const char *path, const char *format, ...)
{
	va_list ap;
	int ret;

	va_start(ap, format);
	ret = vfprintf_new_file(path, format, ap);
	va_end(ap);
	return ret;
}

static int symlink_unit(const char *dir, const char *unit_name)
{
	char *symlink_target = NULL;
	char *symlink_path = NULL;
	int ret;

	ret = asprintf(&symlink_target, "../%s", unit_name);
	if (ret == -1) {
		perror("asprintf");
		symlink_target = NULL;
		goto out;
	}

	ret = asprintf(&symlink_path, "%s/%s", dir, unit_name);
	if (ret == -1) {
		perror("asprintf");
		symlink_path = NULL;
		goto out;
	}

	ret = mkdir(dir, 0755);
	if (ret == -1 && errno != EEXIST) {
		perror("mkdir");
		goto out;
	}

	ret = symlink(symlink_target, symlink_path);
	if (ret == -1 && errno != EEXIST) {
		perror("symlink");
		goto out;
	}

out:
	free(symlink_path);
	free(symlink_target);
	return ret;
}

static int generate_mount_unit(const char *mountpoint, const char *format, ...)
{
	char *unit_name = NULL;
	va_list ap;
	int ret;

	ret = mountpoint_to_unit_name(mountpoint, &unit_name);
	if (ret == -1)
		goto out;

	va_start(ap, format);
	ret = vfprintf_new_file(unit_name, format, ap);
	va_end(ap);
	if (ret == -1)
		goto out;

	ret = symlink_unit("local-fs.target.wants", unit_name);
	if (ret == -1)
		goto out;

out:
	free(unit_name);
	return ret;
}

static int generate_modules_unit(const char *kernelrelease,
				 const char *tmpfs_mountpoint,
				 const char *_9p_mountpoint)
{
	const char *unit_name = "9p-modules.service";
	int ret;

	ret = fprintf_new_file(unit_name,
			       "# Automatically generated by 9p-modules-generator\n"
			       "\n"
			       "[Unit]\n"
			       "Description=Sets up modules mounted via 9p\n"
			       "DefaultDependencies=no\n"
			       "RequiresMountsFor=%s %s\n"
			       "Before=sysinit.target systemd-modules-load.service systemd-udevd.service kmod-static-nodes.service\n"
			       "\n"
			       "[Service]\n"
			       "Type=oneshot\n"
			       "RemainAfterExit=yes\n"
			       "ExecStart=/bin/ln -s build/modules.order /lib/modules/%3$s/modules.order\n"
			       "ExecStart=/bin/ln -s build/modules.builtin /lib/modules/%3$s/modules.builtin\n"
			       "ExecStart=/bin/ln -s build /lib/modules/%3$s/kernel\n"
			       "ExecStart=/sbin/depmod %3$s\n",
			       tmpfs_mountpoint, _9p_mountpoint, kernelrelease);
	if (ret == -1)
		goto out;

	ret = symlink_unit("sysinit.target.wants", unit_name);
	if (ret == -1)
		goto out;

out:
	return ret;
}

static int generate_units(void)
{
	struct utsname name;
	char *tmpfs_mountpoint = NULL;
	char *_9p_mountpoint = NULL;
	int ret;

	ret = uname(&name);
	if (ret == -1) {
		perror("uname");
		goto out;
	}

	ret = asprintf(&tmpfs_mountpoint, "/lib/modules/%s", name.release);
	if (ret == -1) {
		perror("asprintf");
		tmpfs_mountpoint = NULL;
		goto out;
	}

	ret = asprintf(&_9p_mountpoint, "/lib/modules/%s/build", name.release);
	if (ret == -1) {
		perror("asprintf");
		_9p_mountpoint = NULL;
		goto out;
	}

	ret = generate_mount_unit(tmpfs_mountpoint,
				  "# Automatically generated by 9p-modules-generator\n"
				  "\n"
				  "[Unit]\n"
				  "Description=Temporary Modules Directory\n"
				  "DefaultDependencies=no\n"
				  "Conflicts=umount.target\n"
				  "After=systemd-remount-fs.service\n"
				  "Before=local-fs.target umount.target\n"
				  "\n"
				  "[Mount]\n"
				  "What=tmpfs\n"
				  "Where=%s\n"
				  "Type=tmpfs\n"
				  "Options=mode=755,strictatime\n",
				  tmpfs_mountpoint);
	if (ret == -1)
		goto out;

	ret = generate_mount_unit(_9p_mountpoint,
				  "# Automatically generated by 9p-modules-generator\n"
				  "\n"
				  "[Unit]\n"
				  "Description=9p Modules Build Directory\n"
				  "DefaultDependencies=no\n"
				  "Conflicts=umount.target\n"
				  "Before=local-fs.target umount.target\n"
				  "\n"
				  "[Mount]\n"
				  "What=modules\n"
				  "Where=%s\n"
				  "Type=9p\n"
				  "Options=trans=virtio,ro\n",
				  _9p_mountpoint);
	if (ret == -1)
		goto out;

	ret = generate_modules_unit(name.release, tmpfs_mountpoint,
				    _9p_mountpoint);
	if (ret == -1)
		goto out;

out:
	free(_9p_mountpoint);
	free(tmpfs_mountpoint);
	return ret;
}

static int generate_cleanup_unit(void)
{
	const char *unit_name = "9p-modules-cleanup.service";
	int ret;

	ret = fprintf_new_file(unit_name,
			       "# Automatically generated by 9p-modules-generator\n"
			       "\n"
			       "[Unit]\n"
			       "Description=Cleans up empty module directories\n"
			       "DefaultDependencies=no\n"
			       "After=local-fs.target\n"
			       "Before=sysinit.target\n"
			       "\n"
			       "[Service]\n"
			       "Type=oneshot\n"
			       "RemainAfterExit=yes\n"
			       "ExecStart=/usr/bin/find /lib/modules -mindepth 1 -maxdepth 1 -type d -empty -delete\n");
	if (ret == -1)
		goto out;

	ret = symlink_unit("sysinit.target.wants", unit_name);
	if (ret == -1)
		goto out;

out:
	return ret;
}

int main(int argc, char **argv)
{
	char *mount_tag = NULL;
	size_t mount_tag_len = 0;
	glob_t mount_tags;
	size_t i;
	int ret;
	int status = EXIT_FAILURE;

	if (argc != 4) {
		fprintf(stderr, "usage: 9p-modules-generator normal-dir early-dir late-dir\n");
		return EXIT_FAILURE;
	}

	if (chdir(argv[1]) == -1) {
		perror("chdir");
		return EXIT_FAILURE;
	}

	ret = glob("/sys/bus/virtio/drivers/9pnet_virtio/virtio*/mount_tag", 0,
		   NULL, &mount_tags);
	if (ret != 0 && ret != GLOB_NOMATCH)
		goto out;

	for (i = 0; i < mount_tags.gl_pathc; i++) {
		FILE *f;
		ssize_t sret;

		f = fopen(mount_tags.gl_pathv[i], "r");
		if (!f) {
			perror("fopen");
			goto out;
		}

		sret = getdelim(&mount_tag, &mount_tag_len, '\0', f);
		if (sret == -1) {
			perror("getdelim");
			fclose(f);
			goto out;
		}
		fclose(f);

		if (strcmp(mount_tag, "modules") == 0) {
			ret = generate_units();
			if (ret == -1)
				goto out;
			break;
		}
	}

	ret = generate_cleanup_unit();
	if (ret == -1)
		goto out;

	status = EXIT_SUCCESS;
out:
	globfree(&mount_tags);
	free(mount_tag);
	return status;
}
