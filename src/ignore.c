/*
 * Copyright (C) the libgit2 contributors. All rights reserved.
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */

#include "ignore.h"

#include "git2/ignore.h"
#include "common.h"
#include "attrcache.h"
#include "path.h"
#include "config.h"
#include "wildmatch.h"

#define GIT_IGNORE_INTERNAL		"[internal]exclude"

#define GIT_IGNORE_DEFAULT_RULES ".\n..\n.git\n"

static int parse_ignore_file(
	git_repository *repo, git_attr_file *attrs, const char *data, bool allow_macros)
{
	int error = 0;
	int ignore_case = false;
	const char *scan = data, *context = NULL;
	git_attr_fnmatch *match = NULL;

	GIT_UNUSED(allow_macros);

	if (git_repository__configmap_lookup(&ignore_case, repo, GIT_CONFIGMAP_IGNORECASE) < 0)
		git_error_clear();

	/* if subdir file path, convert context for file paths */
	if (attrs->entry &&
		git_path_root(attrs->entry->path) < 0 &&
		!git__suffixcmp(attrs->entry->path, "/" GIT_IGNORE_FILE))
		context = attrs->entry->path;

	if (git_mutex_lock(&attrs->lock) < 0) {
		git_error_set(GIT_ERROR_OS, "failed to lock ignore file");
		return -1;
	}

	while (!error && *scan) {
		if (!match && !(match = git__calloc(1, sizeof(*match)))) {
			error = -1;
			break;
		}

		match->flags =
		    GIT_ATTR_FNMATCH_ALLOWSPACE | GIT_ATTR_FNMATCH_ALLOWNEG;

		if (!(error = git_attr_fnmatch__parse(
			match, &attrs->pool, context, &scan)))
		{
			match->flags |= GIT_ATTR_FNMATCH_IGNORE;

			if (ignore_case)
				match->flags |= GIT_ATTR_FNMATCH_ICASE;

			scan = git__next_line(scan);
			error = git_vector_insert(&attrs->rules, match);
		}

		if (error != 0) {
			match->pattern = NULL;

			if (error == GIT_ENOTFOUND)
				error = 0;
		} else {
			match = NULL; /* vector now "owns" the match */
		}
	}

	git_mutex_unlock(&attrs->lock);
	git__free(match);

	return error;
}

static int push_ignore_file(
	git_ignores *ignores,
	git_vector *which_list,
	const char *base,
	const char *filename)
{
	int error = 0;
	git_attr_file *file = NULL;

	error = git_attr_cache__get(&file, ignores->repo, NULL, GIT_ATTR_FILE__FROM_FILE,
				    base, filename, parse_ignore_file, false);
	if (error < 0)
		return error;

	if (file != NULL) {
		if ((error = git_vector_insert(which_list, file)) < 0)
			git_attr_file__free(file);
	}

	return error;
}

static int push_one_ignore(void *payload, const char *path)
{
	git_ignores *ign = payload;
	ign->depth++;
	return push_ignore_file(ign, &ign->ign_path, path, GIT_IGNORE_FILE);
}

static int parse_ignore_default_rules(
	git_repository *repo, git_attr_file *attrs, const char *data, bool allow_macros) {
  (void)data;
  return parse_ignore_file(repo, attrs, GIT_IGNORE_DEFAULT_RULES, allow_macros);
}

static int get_internal_ignores(git_attr_file **out, git_repository *repo)
{
	int error;

	if ((error = git_attr_cache__init(repo)) < 0)
		return error;

	/* if internal rules list is empty, insert default rules */
	error = git_attr_cache__get(
		out, repo, NULL, GIT_ATTR_FILE__IN_MEMORY, NULL,
    GIT_IGNORE_INTERNAL, parse_ignore_default_rules, false);

	return error;
}

int git_ignore__for_path(
	git_repository *repo,
	const char *path,
	git_ignores *ignores)
{
	int error = 0;
	const char *workdir = git_repository_workdir(repo);
	git_buf infopath = GIT_BUF_INIT;

	assert(repo && ignores && path);

	memset(ignores, 0, sizeof(*ignores));
	ignores->repo = repo;

	/* Read the ignore_case flag */
	if ((error = git_repository__configmap_lookup(
			&ignores->ignore_case, repo, GIT_CONFIGMAP_IGNORECASE)) < 0)
		goto cleanup;

	if ((error = git_attr_cache__init(repo)) < 0)
		goto cleanup;

	/* given a unrooted path in a non-bare repo, resolve it */
	if (workdir && git_path_root(path) < 0) {
		git_buf local = GIT_BUF_INIT;

		if ((error = git_path_dirname_r(&local, path)) < 0 ||
		    (error = git_path_resolve_relative(&local, 0)) < 0 ||
		    (error = git_path_to_dir(&local)) < 0 ||
		    (error = git_buf_joinpath(&ignores->dir, workdir, local.ptr)) < 0)
		{;} /* Nothing, we just want to stop on the first error */
		git_buf_dispose(&local);
	} else {
		error = git_buf_joinpath(&ignores->dir, path, "");
	}
	if (error < 0)
		goto cleanup;

	if (workdir && !git__prefixcmp(ignores->dir.ptr, workdir))
		ignores->dir_root = strlen(workdir);

	/* set up internals */
	if ((error = get_internal_ignores(&ignores->ign_internal, repo)) < 0)
		goto cleanup;

	/* load .gitignore up the path */
	if (workdir != NULL) {
		error = git_path_walk_up(
			&ignores->dir, workdir, push_one_ignore, ignores);
		if (error < 0)
			goto cleanup;
	}

	/* load .git/info/exclude if possible */
	if ((error = git_repository_item_path(&infopath, repo, GIT_REPOSITORY_ITEM_INFO)) < 0 ||
		(error = push_ignore_file(ignores, &ignores->ign_global, infopath.ptr, GIT_IGNORE_FILE_INREPO)) < 0) {
		if (error != GIT_ENOTFOUND)
			goto cleanup;
		error = 0;
	}

	/* load core.excludesfile */
	if (git_repository_attr_cache(repo)->cfg_excl_file != NULL)
		error = push_ignore_file(
			ignores, &ignores->ign_global, NULL,
			git_repository_attr_cache(repo)->cfg_excl_file);

cleanup:
	git_buf_dispose(&infopath);
	if (error < 0)
		git_ignore__free(ignores);

	return error;
}

int git_ignore__push_dir(git_ignores *ign, const char *dir)
{
	if (git_buf_joinpath(&ign->dir, ign->dir.ptr, dir) < 0)
		return -1;

	ign->depth++;

	return push_ignore_file(
		ign, &ign->ign_path, ign->dir.ptr, GIT_IGNORE_FILE);
}

int git_ignore__pop_dir(git_ignores *ign)
{
	if (ign->ign_path.length > 0) {
		git_attr_file *file = git_vector_last(&ign->ign_path);
		const char *start = file->entry->path, *end;

		/* - ign->dir looks something like "/home/user/a/b/" (or "a/b/c/d/")
		 * - file->path looks something like "a/b/.gitignore
		 *
		 * We are popping the last directory off ign->dir.  We also want
		 * to remove the file from the vector if the popped directory
		 * matches the ignore path.  We need to test if the "a/b" part of
		 * the file key matches the path we are about to pop.
		 */

		if ((end = strrchr(start, '/')) != NULL) {
			size_t dirlen = (end - start) + 1;
			const char *relpath = ign->dir.ptr + ign->dir_root;
			size_t pathlen = ign->dir.size - ign->dir_root;

			if (pathlen == dirlen && !memcmp(relpath, start, dirlen)) {
				git_vector_pop(&ign->ign_path);
				git_attr_file__free(file);
			}
		}
	}

	if (--ign->depth > 0) {
		git_buf_rtruncate_at_char(&ign->dir, '/');
		git_path_to_dir(&ign->dir);
	}

	return 0;
}

void git_ignore__free(git_ignores *ignores)
{
	unsigned int i;
	git_attr_file *file;

	git_attr_file__free(ignores->ign_internal);

	git_vector_foreach(&ignores->ign_path, i, file) {
		git_attr_file__free(file);
		ignores->ign_path.contents[i] = NULL;
	}
	git_vector_free(&ignores->ign_path);

	git_vector_foreach(&ignores->ign_global, i, file) {
		git_attr_file__free(file);
		ignores->ign_global.contents[i] = NULL;
	}
	git_vector_free(&ignores->ign_global);

	git_buf_dispose(&ignores->dir);
}

static bool ignore_lookup_in_rules(
	int *ignored, git_attr_file *file, git_attr_path *path)
{
	size_t j;
	git_attr_fnmatch *match;

	git_vector_rforeach(&file->rules, j, match) {
		if (match->flags & GIT_ATTR_FNMATCH_DIRECTORY &&
		    path->is_dir == GIT_DIR_FLAG_FALSE)
			continue;
		if (git_attr_fnmatch__match(match, path)) {
			*ignored = ((match->flags & GIT_ATTR_FNMATCH_NEGATIVE) == 0) ?
				GIT_IGNORE_TRUE : GIT_IGNORE_FALSE;
			return true;
		}
	}

	return false;
}

int git_ignore__lookup(
	int *out, git_ignores *ignores, const char *pathname, git_dir_flag dir_flag)
{
	size_t i;
	git_attr_file *file;
	git_attr_path path;

	*out = GIT_IGNORE_NOTFOUND;

	if (git_attr_path__init(
		&path, pathname, git_repository_workdir(ignores->repo), dir_flag) < 0)
		return -1;

	/* first process builtins - success means path was found */
	if (ignore_lookup_in_rules(out, ignores->ign_internal, &path))
		goto cleanup;

	/* next process files in the path.
	 * this process has to process ignores in reverse order
	 * to ensure correct prioritization of rules
	 */
	git_vector_rforeach(&ignores->ign_path, i, file) {
		if (ignore_lookup_in_rules(out, file, &path))
			goto cleanup;
	}

	/* last process global ignores */
	git_vector_foreach(&ignores->ign_global, i, file) {
		if (ignore_lookup_in_rules(out, file, &path))
			goto cleanup;
	}

cleanup:
	git_attr_path__free(&path);
	return 0;
}

int git_ignore_add_rule(git_repository *repo, const char *rules)
{
	int error;
	git_attr_file *ign_internal = NULL;

	if ((error = get_internal_ignores(&ign_internal, repo)) < 0)
		return error;

	error = parse_ignore_file(repo, ign_internal, rules, false);
	git_attr_file__free(ign_internal);

	return error;
}

int git_ignore_clear_internal_rules(git_repository *repo)
{
	int error;
	git_attr_file *ign_internal;

	if ((error = get_internal_ignores(&ign_internal, repo)) < 0)
		return error;

	if (!(error = git_attr_file__clear_rules(ign_internal, true)))
		error = parse_ignore_file(
				repo, ign_internal, GIT_IGNORE_DEFAULT_RULES, false);

	git_attr_file__free(ign_internal);
	return error;
}

int git_ignore_path_is_ignored(
	int *ignored,
	git_repository *repo,
	const char *pathname)
{
	int error;
	const char *workdir;
	git_attr_path path;
	git_ignores ignores;
	unsigned int i;
	git_attr_file *file;
	git_dir_flag dir_flag = GIT_DIR_FLAG_UNKNOWN;

	assert(repo && ignored && pathname);

	workdir = git_repository_workdir(repo);

	memset(&path, 0, sizeof(path));
	memset(&ignores, 0, sizeof(ignores));

	if (!git__suffixcmp(pathname, "/"))
		dir_flag = GIT_DIR_FLAG_TRUE;
	else if (git_repository_is_bare(repo))
		dir_flag = GIT_DIR_FLAG_FALSE;

	if ((error = git_attr_path__init(&path, pathname, workdir, dir_flag)) < 0 ||
		(error = git_ignore__for_path(repo, path.path, &ignores)) < 0)
		goto cleanup;

	while (1) {
		/* first process builtins - success means path was found */
		if (ignore_lookup_in_rules(ignored, ignores.ign_internal, &path))
			goto cleanup;

		/* next process files in the path */
		git_vector_foreach(&ignores.ign_path, i, file) {
			if (ignore_lookup_in_rules(ignored, file, &path))
				goto cleanup;
		}

		/* last process global ignores */
		git_vector_foreach(&ignores.ign_global, i, file) {
			if (ignore_lookup_in_rules(ignored, file, &path))
				goto cleanup;
		}

		/* move up one directory */
		if (path.basename == path.path)
			break;
		path.basename[-1] = '\0';
		while (path.basename > path.path && *path.basename != '/')
			path.basename--;
		if (path.basename > path.path)
			path.basename++;
		path.is_dir = 1;

		if ((error = git_ignore__pop_dir(&ignores)) < 0)
			break;
	}

	*ignored = 0;

cleanup:
	git_attr_path__free(&path);
	git_ignore__free(&ignores);
	return error;
}

int git_ignore__check_pathspec_for_exact_ignores(
	git_repository *repo,
	git_vector *vspec,
	bool no_fnmatch)
{
	int error = 0;
	size_t i;
	git_attr_fnmatch *match;
	int ignored;
	git_buf path = GIT_BUF_INIT;
	const char *wd, *filename;
	git_index *idx;

	if ((error = git_repository__ensure_not_bare(
			repo, "validate pathspec")) < 0 ||
		(error = git_repository_index(&idx, repo)) < 0)
		return error;

	wd = git_repository_workdir(repo);

	git_vector_foreach(vspec, i, match) {
		/* skip wildcard matches (if they are being used) */
		if ((match->flags & GIT_ATTR_FNMATCH_HASWILD) != 0 &&
			!no_fnmatch)
			continue;

		filename = match->pattern;

		/* if file is already in the index, it's fine */
		if (git_index_get_bypath(idx, filename, 0) != NULL)
			continue;

		if ((error = git_buf_joinpath(&path, wd, filename)) < 0)
			break;

		/* is there a file on disk that matches this exactly? */
		if (!git_path_isfile(path.ptr))
			continue;

		/* is that file ignored? */
		if ((error = git_ignore_path_is_ignored(&ignored, repo, filename)) < 0)
			break;

		if (ignored) {
			git_error_set(GIT_ERROR_INVALID, "pathspec contains ignored file '%s'",
				filename);
			error = GIT_EINVALIDSPEC;
			break;
		}
	}

	git_index_free(idx);
	git_buf_dispose(&path);

	return error;
}

