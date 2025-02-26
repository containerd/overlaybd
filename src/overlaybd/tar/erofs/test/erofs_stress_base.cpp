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

#include "erofs_stress_base.h"
#include <ctime>
#include <chrono>
#include <sys/stat.h>
#include <dirent.h>
#include <algorithm>
#include <random>
#include "../liberofs.h"
#include "../erofs_fs.h"
#include "../../../../tools/comm_func.h"

#define get_randomint(a, b) ((rand() % (b - a)) + a)

std::string get_randomstr(int max_length, bool range)
{
	const char chs[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
	int len = strlen(chs);
	int length = range ? rand() % max_length + 1 : max_length;
	std::string res;

	for (int i = 0; i < length; i ++) {
		res.push_back(chs[std::rand() % len]);
	}
	return res;
}
bool is_substring(const std::string& str, const std::string& substring) {
    return str.find(substring) != std::string::npos;
}

bool str_n_equal(std::string s1, std::string s2, long unsigned int n) {
	if (s1.length() < n || s2.length() < n)
			return false;
	return s1.substr(0, n) == s2.substr(0, n);
}

bool StressFsTree::add_node(StressNode *node) {
	if (!node || !node->path.size() || node->type >= NODE_TYPE_MAX)
		LOG_ERRNO_RETURN(-1, false, "invalid node");

	if (node->type != NODE_WHITEOUT) {
		/* the upper regular file should remove the lower dir */
		std::map<std::string, StressNode*>::iterator dir_it;
		if (node->type == NODE_REGULAR && (dir_it = tree.find(node->path)) != tree.end() &&
			dir_it->second->type == NODE_DIR)
		{
			tree.erase(dir_it);
			std::string rm_prefix = node->path + "/";
			for (auto it = tree.begin(); it != tree.end(); ) {
				if (str_n_equal(rm_prefix, it->first, rm_prefix.length())) {
					it = tree.erase(it);
				} else {
					++it;
				}

			}
		}
		tree[node->path] = node;
	} else {
		auto it = tree.find(node->path);
		if (it == tree.end() || it->second->type == NODE_WHITEOUT)
			LOG_ERROR_RETURN(-1, false, "whiteout a invalid object");
		/* whiteout a regular file */
		if (it->second->type == NODE_REGULAR) {
			tree.erase(it);
		} else if (it->second->type == NODE_DIR) {
			/* whiteout a dir */
			std::string rm_prefix = it->first + "/";
			tree.erase(it);
			for (auto p = tree.begin(); p != tree.end(); ) {
				if (str_n_equal(rm_prefix, p->first, rm_prefix.length()))
					p = tree.erase(p);
				else
					p ++;
			}
		} else
			LOG_ERROR_RETURN(-1, false, "invalid object type: `", it->second->type);
	}
	return true;
}

std::string StressFsTree::get_same_name(int idx, int depth, std::string root_path, NODE_TYPE type, bool same_type) {
	std::vector<std::string> vec;
	for (const auto& pair : tree) {
		if (pair.first == "/" || !is_substring(pair.first, root_path) ||
		    pair.first.length() == root_path.length())
			continue;
		if (same_type && pair.second->type != type)
			continue;
		std::string last_component = pair.first.substr(root_path.length() + 1);
		if (!is_substring(last_component, "/"))
			vec.emplace_back(last_component);
	}

	if (vec.empty())
		return get_randomstr(type ? MAX_FILE_NAME : MAX_DIR_NAME, true);

	std::random_device rd;
	std::default_random_engine engine(rd());
	std::shuffle(vec.begin(), vec.end(), engine);

	return vec[0];
}

NODE_TYPE StressFsTree::get_type(std::string root_path) {
	std::map<std::string, StressNode*>::iterator it;

	it = tree.find(root_path);
	if (it == tree.end())
		return NODE_TYPE_MAX;
	return it->second->type;
}

struct LayerNode {
	std::string pwd;
	std::vector<LayerNode*> subdirs;
	int num_files, depth;
};

static LayerNode *build_layer_tree(std::vector<int> &dirs) {
	std::vector<LayerNode*> nodes;

	for (int i = 0; i < (int)dirs.size(); i ++) {
		nodes.emplace_back(new LayerNode);
		nodes[i]->depth = 0;
		nodes[i]->num_files = dirs[i];
	}

	while (nodes.size() > 1) {
		int idx = rand() % nodes.size();
		LayerNode *cur = nodes[idx];

		nodes.erase(nodes.begin() + idx);
		idx = rand() % nodes.size();
		nodes[idx]->subdirs.emplace_back(cur);
	}

	return nodes[0];
}

static bool append_tar(bool first, std::string tar_name, std::string tmp_tar, std::string prefix, std::string file_name, struct in_mem_meta *meta)
{
	std::string cmd = std::string("tar --create --file=") + (first ? tar_name : tmp_tar) + " --xattrs --xattrs-include='*'";
	if (meta)
		cmd = cmd + " --owner=" + std::to_string(meta->uid) + " --group=" + std::to_string(meta->gid) + " --mtime=\"" + meta->mtime_date + "\"";
	cmd = cmd + " -C " + prefix + " " + file_name;

	if (system(cmd.c_str()))
		LOG_ERROR_RETURN(-1, false, "fail to create tar file for `, cmd: `", prefix + "/" + file_name, cmd);
	if (!first) {
		cmd = std::string("tar --concatenate --file=") + tar_name + " " + tmp_tar;
		if (system(cmd.c_str()))
			LOG_ERROR_RETURN(-1, false, "fail to concatenate ` to `, cmd: `", tmp_tar, tar_name, cmd);
	}
	return true;
}

bool StressBase::create_layer(int idx) {

#define MAX_TRY_TIME 10

	std::string origin_prefix = prefix;
	// add a random prefix to avoid operations in the same dir
	prefix = prefix + "/" + get_randomstr(20, false);
	if (host_fs->mkdir(prefix.c_str(), 0755) != 0)
		LOG_ERROR_RETURN(-1, false, "fail to prepare for the current workdir `", prefix);

	std::vector<int> dirs = layer_dirs(idx);
	LayerNode *layer_tree = build_layer_tree(dirs);
	std::vector<LayerNode*> q;
	std::string root_dirname = generate_name(idx, layer_tree->depth, "", NODE_DIR);
	std::string root_path = prefix + "/" + root_dirname;
	std::string clean_cmd = "rm -rf " + root_path;
	bool res;
	std::map<std::string, struct in_mem_meta *> meta_maps;

	if (system(clean_cmd.c_str()))
		LOG_ERROR_RETURN(-1, false, "fail to prepare clean dir for `", root_path);

	layer_tree->pwd = root_path;
	q.emplace_back(layer_tree);

	std::string layer_name = origin_prefix + "/layer" + std::to_string(idx) + ".tar";
	std::string tmp_tar = origin_prefix + "/layer" + std::to_string(idx) + "_tmp.tar";

	StressNode *node = new StressNode(layer_tree->pwd.substr(prefix.length()), NODE_DIR);
	if (host_fs->mkdir(layer_tree->pwd.c_str(), 0755) != 0)
		LOG_ERROR_RETURN(-1, false, "fail to mkdir `", layer_tree->pwd);

	meta_maps[layer_tree->pwd] = new struct in_mem_meta();
	res = build_dir_mod(node, layer_tree->pwd.c_str(), host_fs) &&
		build_dir_own(node, meta_maps[layer_tree->pwd]) &&
		build_dir_mtime(node, meta_maps[layer_tree->pwd]) &&
		build_dir_xattrs(node, layer_tree->pwd.c_str(), host_fs) &&
		build_stat_dir(node, layer_tree->pwd.c_str(), host_fs, meta_maps[layer_tree->pwd]);
	if (!res)
		LOG_ERROR_RETURN(-1, false, "fail to generate fields for dir `",layer_tree->pwd);
	if (!tree->add_node(node))
		LOG_ERROR_RETURN(-1, false, "fail to add node `",layer_tree->pwd);

	if (!append_tar(true, layer_name, tmp_tar, prefix, root_dirname, meta_maps[layer_tree->pwd]))
		LOG_ERROR_RETURN(-1, false, "fail to crate tar for `", layer_tree->pwd);

	// traverse the layer tree
	while (q.size()) {
		LayerNode *cur = q.front();
		q.erase(q.begin());

		for (int i = 0; i < (int)cur->num_files; i ++) {
			std::string name_prefix = cur->pwd.substr(prefix.length());
			// generate filename for files in the current dir
			std::string filename = generate_name(idx, cur->depth, name_prefix, NODE_REGULAR);
			/* if this is a whout file */
			if (str_n_equal(filename, std::string(EROFS_WHOUT_PREFIX), strlen(EROFS_WHOUT_PREFIX))) {
				std::string host_filename = name_prefix + "/" + filename;
				filename = name_prefix + "/" + filename.substr(strlen(EROFS_WHOUT_PREFIX));
				if (tree->get_type(filename) != NODE_REGULAR)
					LOG_ERROR_RETURN(-1, false, "invalid whiteout filename: `", filename);

				StressHostFile *file_info = new StressHostFile(prefix + host_filename, host_fs);
				if (!file_info->file)
					LOG_ERROR_RETURN(-1, false, "fail to crate whiteout file in host fs: `", host_filename);

				StressNode *node = new StressNode(filename, NODE_WHITEOUT);
				if (!tree->add_node(node))
					LOG_ERROR_RETURN(-1, false, "fail to add WHITEOUT file `", filename);
				file_info->file->fsync();
				delete file_info;
				if (!append_tar(false, layer_name, tmp_tar, prefix, host_filename.substr(1), nullptr))
					LOG_ERROR_RETURN(-1, false, "fail to create tar for whiteout file: `", (prefix + host_filename));
			} else {
				filename = name_prefix + "/" + filename;
				StressNode *node = new StressNode(filename, NODE_REGULAR);
				StressHostFile *file_info = new StressHostFile(prefix + filename, host_fs);

				meta_maps[prefix + filename] = new struct in_mem_meta();
				res = build_gen_mod(node, file_info) &&
					  build_gen_own(node, meta_maps[prefix + filename]) &&
					  build_gen_mtime(node, meta_maps[prefix + filename]) &&
					  build_gen_xattrs(node, file_info) &&
					  build_gen_content(node, file_info) &&
					  build_stat_file(node, file_info, meta_maps[prefix + filename]);
				if (!res)
					LOG_ERROR_RETURN(-1, false, "fail to generate file contents");
				if (!tree->add_node(node))
					LOG_ERROR_RETURN(-1, false, "failt to add node `", filename);
				file_info->file->fsync();
				delete file_info;
				if (!append_tar(false, layer_name, tmp_tar, prefix, filename.substr(1), meta_maps[prefix + filename]))
					LOG_ERROR_RETURN(-1, false, "fail to create tar for file `", (prefix + filename));
			}
		}

		for (int i = 0; i < (int)cur->subdirs.size(); i ++) {
			LayerNode *next = cur->subdirs[i];
			next->depth = cur->depth + 1;
			// generate subdir name in the current dir
			for (int try_times = 0; try_times < MAX_TRY_TIME; try_times++) {
				std::string dir_name = generate_name(idx, cur->depth, cur->pwd.substr(prefix.length()), NODE_DIR);

				/* if it is a whout dir */
				if (str_n_equal(dir_name, std::string(EROFS_WHOUT_PREFIX), strlen(EROFS_WHOUT_PREFIX))) {
					std::string host_filename = cur->pwd + "/" + dir_name;
					next->pwd = cur->pwd + "/" + dir_name.substr(strlen(EROFS_WHOUT_PREFIX));
					if (tree->get_type(next->pwd.substr(prefix.length())) != NODE_DIR)
						LOG_ERROR_RETURN(-1, false, "invalid whiteout dir name: `", next->pwd.substr(prefix.length()));
					StressNode *dir_node = new StressNode(next->pwd.substr(prefix.length()), NODE_WHITEOUT);
					if (!tree->add_node(dir_node))
						LOG_ERROR_RETURN(-1, false, "fail to add WHITEOUT dir `", next->pwd.substr(prefix.length()));
					/* create a .wh.dirname in the host fs */
					StressHostFile *file_info = new StressHostFile(host_filename, host_fs);
					if (!file_info->file)
						LOG_ERROR_RETURN(-1, false, "fail to crate whiout dir in host fs: `", host_filename);
					file_info->file->fsync();
					delete file_info;
					if (!append_tar(false, layer_name, tmp_tar, prefix, host_filename.substr(prefix.length() + 1), nullptr))
						LOG_ERROR_RETURN(-1, false, "fail to create whitout dir for `", host_filename);
					break;
				} else {
					next->pwd = cur->pwd + "/" + dir_name;
					if (host_fs->mkdir(next->pwd.c_str(), 0755) == 0) {
						StressNode *dir_node = new StressNode(next->pwd.substr(prefix.length()), NODE_DIR);

						meta_maps[next->pwd] = new struct in_mem_meta();
						res = build_dir_mod(dir_node, next->pwd.c_str(), host_fs) &&
							build_dir_own(dir_node, meta_maps[next->pwd]) &&
							build_dir_mtime(dir_node, meta_maps[next->pwd]) &&
							build_dir_xattrs(dir_node, next->pwd.c_str(), host_fs) &&
							build_stat_dir(dir_node, next->pwd.c_str(), host_fs, meta_maps[next->pwd]);
						if (!res)
							LOG_ERROR_RETURN(-1, false, "fail to generate fields for dir `", next->pwd);
						if (!tree->add_node(dir_node))
							LOG_ERROR_RETURN(-1, false, "fail to add node `", next->pwd);
						q.emplace_back(next);
						if (!append_tar(false, layer_name, tmp_tar, prefix, next->pwd.substr(prefix.length() + 1), meta_maps[next->pwd]))
							LOG_ERROR_RETURN(-1, false, "fail to create tar for dir `", next->pwd);
						break;
					}
				}
			}
		}
		delete cur;
	}

#undef MAX_TRY_TIME

	for (auto it = meta_maps.begin(); it != meta_maps.end(); ) {
		delete it->second;
		it = meta_maps.erase(it);
	}

	prefix = origin_prefix;
	return true;
}

LSMT::IFileRW *StressBase::mkfs()
{
	LSMT::IFileRW *lowers = nullptr;
	for (int i = 0; i < num_layers; i ++) {
		LOG_INFO("processing layer `", i);
		std::string src_path = prefix + "/layer" + std::to_string(i) + ".tar";
		std::string idx_path = prefix + "/layer" + std::to_string(i) + ".idx";
		std::string meta_path = prefix + "/layer" + std::to_string(i) + ".meta";

		/* prepare for idx and meta files */
		auto src_file = host_fs->open(src_path.c_str(), O_RDONLY, 0666);
		auto idx_file = host_fs->open(idx_path.c_str(), O_RDWR | O_CREAT | O_TRUNC, S_IRWXU);
		auto meta_file = host_fs->open(meta_path.c_str(), O_RDWR | O_CREAT | O_TRUNC, S_IRWXU);
		if (!src_file || !idx_file || !meta_file)
			LOG_ERROR_RETURN(-1, nullptr, "fail to prepare tar, idx or meta file for layer `", std::to_string(i));

		LSMT::WarpFileArgs args(idx_file, meta_file, src_file);
		args.virtual_size = IMAGE_SIZE;
		LSMT::IFileRW *current_layer = create_warpfile(args, false);
		if (!current_layer)
			LOG_ERROR_RETURN(-1, nullptr, "fail to prepare wrapfile for layer `", std::to_string(i));

		LSMT::IFileRW *img_file = nullptr;
		if (i > 0)
			img_file = LSMT::stack_files(current_layer, lowers, true, false);
		else
			img_file = current_layer;

		/* create erofs fs image */
		auto tar = new LibErofs(img_file, 4096, false);
		if (tar->extract_tar(src_file, true, i == 0)) {
			delete img_file;
			LOG_ERROR_RETURN(-1, nullptr, "fail to extract tar");
		}
		delete tar;
		lowers = img_file;
	}

	return lowers;
}

bool StressBase::verify(photon::fs::IFileSystem *erofs_fs) {
	std::vector<std::string> items;
	std::string cur;
	struct stat st;
	bool first = true;

	items.emplace_back(std::string("/"));
	do {
		StressNode *node;
		cur = items.front();
		items.erase(items.begin());

		if (erofs_fs->stat(cur.c_str(), &st))
			LOG_ERRNO_RETURN(-1, false, "fail to stat file `", cur);
		if (S_ISDIR(st.st_mode)) {
			auto dir = erofs_fs->opendir(cur.c_str());
			/* the dir may be empty, so check it first */
			if (dir->get() != nullptr) {
				do {
					dirent *dent = dir->get();

					if (first)
						items.emplace_back(cur + std::string(dent->d_name));
					else
						items.emplace_back(cur + "/" + std::string(dent->d_name));
				} while (dir->next());
			}
			dir->closedir();
			delete dir;
		}
		node = new StressNode(cur, S_ISREG(st.st_mode) ? NODE_REGULAR : NODE_DIR);

		if (!first) {
			photon::fs::IFile *file;
			bool ret;

			file = erofs_fs->open(cur.c_str(), O_RDONLY);
			if (!file || ! node)
				LOG_ERROR_RETURN(0, false, "fail to open file or node `", cur);
			ret = verify_gen_xattrs(node, file) &&
			      verify_stat(node, file);
			/* do not generate contents for dirs */
			if (S_ISREG(st.st_mode))
				ret += verify_gen_content(node, file);
			if (!ret)
				LOG_ERROR_RETURN(0, false, "fail to construct StressNode");
			file->close();
			delete file;

		}

		if (!tree->query_delete_node(node)) {
			delete node;
			LOG_ERROR_RETURN(-1, false, "file ` in erofs_fs but not in the in-mem tree", cur);
		}
		first = false;

	} while (!items.empty());

	if (!tree->is_emtry())
		LOG_ERROR_RETURN(-1, false, "Mismatch: in-mem tree is not empty!");
	return true;
}

bool StressBase::run()
{
	if (workdir_exists)
	   LOG_ERROR_RETURN(-1, false, "workdir already exists: `", prefix);

	if (!tree->add_node(new StressNode("/", NODE_DIR)))
		LOG_ERROR_RETURN(-1, false, "fail to add root node into in-mem tree");

	for (int i = 0; i < num_layers; i ++) {
		if (!create_layer(i))
			LOG_ERRNO_RETURN(-1, false, "fail to crate layer `", std::to_string(i));
	}

	LSMT::IFileRW *lowers = mkfs();
	if (!lowers)
		LOG_ERROR_RETURN(-1, false, "failt to mkfs");

	auto erofs_fs = create_erofs_fs(lowers, 4096);
	if (!erofs_fs)
		LOG_ERROR_RETURN(-1, false, "failt to crate erofs fs");

	bool ret = verify(erofs_fs);

	if (ret) {
		std::string clear_cmd = std::string("rm -rf ") + prefix;
		if (system(clear_cmd.c_str()))
			LOG_ERROR_RETURN(-1, false, "fail to clear tmp workdir, cmd: `", clear_cmd);

	}

	return ret;
}
