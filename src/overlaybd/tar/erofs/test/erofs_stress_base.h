/*
   Copyright The Overlaybd Authors

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

	   http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/


#pragma once

#include <string>
#include <map>
#include <photon/fs/filesystem.h>
#include <photon/fs/localfs.h>
#include <photon/common/alog.h>
#include <gtest/gtest.h>
#include <fcntl.h>
#include "../../../lsmt/file.h"
#include "../erofs_fs.h"

#define IMAGE_SIZE 1UL<<30
#define SECTOR_SIZE 512ULL
#define MAX_DIR_NAME 100
#define MAX_FILE_NAME 100

#define EROFS_WHOUT_PREFIX ".wh."

/* in-mem node type */
enum NODE_TYPE {
	NODE_DIR,
	NODE_REGULAR,
	NODE_WHITEOUT,
	NODE_TYPE_MAX
};

class StressNode {
public:
	/* meta info for a in-mem node */
	std::string path;
	std::map<std::string, std::string> xattrs;
	std::string content;
	enum NODE_TYPE type;
	struct stat node_stat;

	StressNode(std::string _path, NODE_TYPE _type): path(_path), type(_type) {
		memset(&node_stat, 0, sizeof(node_stat));
		if (type == NODE_DIR)
			node_stat.st_nlink = 2;
	}

	StressNode(StressNode *ano):
		path(ano->path), xattrs(ano->xattrs), content(ano->content) { }

	bool equal(StressNode *ano) {
		if (!ano)
			LOG_ERROR_RETURN(-1,false, "invalid ano: nullptr");
		if (xattrs.size() != ano->xattrs.size()) {
			LOG_INFO("current: `", path);
			for (const auto& pair: xattrs) {
				LOG_INFO("key: `, value: `", pair.first, pair.second);
			}
			LOG_INFO("ano: `", ano->path);
			for (const auto& pair: ano->xattrs) {
				LOG_INFO("key: `, value: `", pair.first, pair.second);
			}
			LOG_ERROR_RETURN(-1,false, "xattrs size not equal: ` != `", xattrs.size(), ano->xattrs.size());
		}
		for (auto it = xattrs.begin(); it != xattrs.end(); it ++) {
			auto p = ano->xattrs.find(it->first);
			if (p == ano->xattrs.end())
				LOG_ERROR_RETURN(-1, false, "xattr ` not in ano", it->first);
			if (p->second.compare(it->second))
				LOG_ERROR_RETURN(-1, false, "xattr ` not equal: ` not equal to `", it->first, p->second, it->second);
			if (p == ano->xattrs.end() || p->second.compare(it->second))
				LOG_ERROR_RETURN(-1, false, "xattr ` not equal", it->first);
		}

		if (path.compare(ano->path))
			LOG_ERROR_RETURN(-1, false, " path ` not equal to ` (`)", path, ano->path, path);
		if (content.compare(ano->content))
			LOG_ERROR_RETURN(-1, false, "content ` not equal to ` (`)", content, ano->content, path);
		if (type != ano->type)
			LOG_ERROR_RETURN(-1, false, "type ` not equal to ` (`)", type, ano->type, path);
		if (node_stat.st_mode != ano->node_stat.st_mode)
			LOG_ERROR_RETURN(-1, false, "mode ` not equal to ` (`)", node_stat.st_mode, ano->node_stat.st_mode, path);
		if (node_stat.st_uid != ano->node_stat.st_uid)
			LOG_ERROR_RETURN(-1, false, "uid ` not equal to ` (`)", node_stat.st_uid, ano->node_stat.st_uid, path);
		if (node_stat.st_gid != ano->node_stat.st_gid)
			LOG_ERROR_RETURN(-1, false, "gid ` not equal to ` (`)", node_stat.st_gid, ano->node_stat.st_gid, path);
		/*
		 * We do not compare the directory's nlink because
		 * different directories are used when creating the layer.
		 */
		if (type != NODE_DIR && node_stat.st_nlink != ano->node_stat.st_nlink)
			LOG_ERROR_RETURN(-1, false, "nlink ` not equal to ` (`)", node_stat.st_nlink, ano->node_stat.st_nlink, path);
		/*
		 * The host file system (e.g., ext4) has a different
		 * directory organization than EROFS, so the directory's
		 * `st_size` is not compared.
		 */
		if (type != NODE_DIR && node_stat.st_size != ano->node_stat.st_size)
			LOG_ERROR_RETURN(-1, false, "file size ` not equal to ` (`)", node_stat.st_size, ano->node_stat.st_size, path);

		if (node_stat.st_mtime != ano->node_stat.st_mtime)
			LOG_ERRNO_RETURN(-1, false, "mtime ` not equal to ` (`)", node_stat.st_mtime,
										  ano->node_stat.st_mtime,
										  path);
		return true;
	}
};

/* a file in the host fs */
class StressHostFile {
public:
	std::string path;
	photon::fs::IFile *file;
	StressHostFile() {
		file = nullptr;
	}

	StressHostFile(std::string _path, photon::fs::IFileSystem *fs):
		path(_path)
	{
		file = fs->open(_path.c_str(), O_RDWR | O_CREAT | O_TRUNC, S_IRWXU);
		if (!file)
			LOG_ERROR("fail to open file `", _path);
	}

	~StressHostFile() {
		file->close();
		delete file;
	}
};

struct in_mem_meta{
	uid_t uid;
	gid_t gid;
	std::string mtime_date;
	time_t mtime;
};

/* interface to generate corresponding values for in-mem nodes and host-fs files */
class StressGenInter {
public:
	/* for a single file (node) */
	/* generate content for in-memory inodes and host files (prepare layers phase) */
	virtual bool build_gen_mod(StressNode *node /* out */, StressHostFile *file_info /* out */) = 0;
	virtual bool build_gen_own(StressNode *node /* out */, struct in_mem_meta *meta /* out */) = 0;
	virtual bool build_gen_mtime(StressNode *node, struct in_mem_meta *meta) = 0;
	virtual bool build_gen_xattrs(StressNode *node /* out */, StressHostFile *file_info /* out */) = 0;
	virtual bool build_gen_content(StressNode *node /* out */, StressHostFile *file_info /* out */) = 0;
	virtual bool build_stat_file(StressNode *node /* out */, StressHostFile *file_info /* out */, struct in_mem_meta *meta) = 0;

	/* for a single dir (node) */
	virtual bool build_dir_mod(StressNode *node,  const char *path, photon::fs::IFileSystem *host_fs) = 0;
	virtual bool build_dir_own(StressNode *node, struct in_mem_meta *meta) = 0;
	virtual bool build_dir_mtime(StressNode *node, struct in_mem_meta *meta) = 0;
	virtual bool build_dir_xattrs(StressNode *node, const char *path, photon::fs::IFileSystem *host_fs) = 0;
	virtual bool build_stat_dir(StressNode *node, const char *path, photon::fs::IFileSystem *host_fs, struct in_mem_meta *meta) = 0;

	/* generate in-mem inode according to erofs-fs file (for both files and dirs) */
	virtual bool verify_gen_xattrs(StressNode *node /* out */, photon::fs::IFile *erofs_file /* in */) = 0;
	virtual bool verify_gen_content(StressNode *node /* out */, photon::fs::IFile *erofs_file /* in */) = 0;
	virtual bool verify_stat(StressNode *node /* out */, photon::fs::IFile *erofs_file /* in */) = 0;

	/*
	 * construct the structure of a layer, such as how many dirs,
	 * how many files are in each directory, etc.
	 */
	virtual std::vector<int> layer_dirs(int idx) = 0;

	/*
	 * generate the name for a dir or a file, to:
	 * 1. control the overlap between the upper layer and lower layer
	 * 2. generate .wh.*
	 */
	virtual std::string generate_name(int idx, int depth, std::string root_path, NODE_TYPE type) = 0;

	virtual ~StressGenInter() {}
};

/*
 * the in-mem fs tree, which is used
 * to do the final verification work.
 */
class StressFsTree {
private:
	std::map<std::string, StressNode*> tree;
public:
	StressFsTree() {}
	~StressFsTree() {
		for (auto it = tree.begin(); it != tree.end();) {
			StressNode *node = it->second;
			it = tree.erase(it);
			delete node;
		}
	}

	// build process
	bool add_node(StressNode *node);

	// verify process
	bool query_delete_node(StressNode *node) {
		if (!node)
			LOG_ERROR_RETURN(-1, false, "invalid node: nullptr");
		auto it = tree.find(node->path);
		if (it == tree.end())
			LOG_ERROR_RETURN(-1,false, "path ` does not exist in in-mem tree", node->path);
		if (!it->second)
			LOG_ERROR_RETURN(-1, false, "NULL in-mem info (`)", node->path);
		if (!it->second->equal(node))
			LOG_ERROR_RETURN(-1, false, "node contents mismatch");
		tree.erase(it);
		return true;
	}

	bool is_emtry() {
		return tree.empty();
	}

	std::string get_same_name(int idx, int depth, std::string root_path, NODE_TYPE type, bool same_type = false);
	NODE_TYPE get_type(std::string root_path);
};

class StressBase: public StressGenInter {
public:
	StressFsTree *tree;

	StressBase(std::string path, int num): prefix(path), num_layers(num) {
		host_fs = photon::fs::new_localfs_adaptor();
		if (!host_fs)
			LOG_ERROR("fail to create host_fs");
		if (host_fs->access(path.c_str(), 0) == 0)
			workdir_exists = true;
		else {
			workdir_exists = false;
			if (host_fs->mkdir(path.c_str(), 0755))
				LOG_ERROR("fail to create dir `", path);
		}
		tree = new StressFsTree();
		if (!host_fs || !tree)
			LOG_ERROR("fail to init StressBase");
	}

	~StressBase() {
		delete host_fs;
		delete tree;
	}

	bool run();
private:
	std::string prefix;
	int num_layers;
	photon::fs::IFileSystem *host_fs;
	bool workdir_exists;

	bool create_layer(int idx);
	LSMT::IFileRW *mkfs();
	bool verify(photon::fs::IFileSystem *erofs_fs);
};

/* helper functions */
std::string get_randomstr(int max_length, bool range);
#define get_randomint(a, b) ((rand() % (b - a)) + a)
bool is_substring(const std::string& str, const std::string& substring);
bool str_n_equal(std::string s1, std::string s2, long unsigned int n);
