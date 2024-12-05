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

bool StressBase::create_layer(int idx) {
	std::vector<int> dirs = layer_dirs(idx);
	LayerNode *layer_tree = build_layer_tree(dirs);
	std::vector<LayerNode*> q;
	std::string root_dirname = generate_name(idx, layer_tree->depth, "", NODE_DIR);
	std::string root_path = prefix + "/" + root_dirname;
	std::string clean_cmd = "rm -rf " + root_path;

	if (system(clean_cmd.c_str()))
		LOG_ERROR_RETURN(-1, false, "fail to prepare clean dir for `", root_path);

	layer_tree->pwd = root_path;
	q.emplace_back(layer_tree);

	// traverse the layer tree
	while (q.size()) {
		bool res;
		LayerNode *cur = q.front();
		q.erase(q.begin());

		StressNode *node = new StressNode(cur->pwd.substr(prefix.length()), NODE_DIR);
		if (host_fs->mkdir(cur->pwd.c_str(), 0755) != 0)
			LOG_ERROR_RETURN(-1, false, "fail to mkdir `", cur->pwd);
		tree->add_node(node);

		for (int i = 0; i < (int)cur->num_files; i ++) {
			std::string name_prefix = cur->pwd.substr(prefix.length());
			// generate filename for files in the current dir
			std::string filename = name_prefix + "/" + generate_name(idx, cur->depth, name_prefix, NODE_REGULAR);
			StressNode *node = new StressNode(filename, NODE_REGULAR);
			StressHostFile *file_info = new StressHostFile(prefix + filename, host_fs);

			res = build_gen_mod(node, file_info) &&
				  build_gen_own(node, file_info) &&
				  build_gen_xattrs(node, file_info) &&
				  build_gen_content(node, file_info);
			if (!res)
				LOG_ERROR_RETURN(-1, false, "fail to generate file contents");
			if (!tree->add_node(node))
				LOG_ERROR_RETURN(-1, false, "failt to add node `", filename);
			file_info->file->fsync();
		}

		for (int i = 0; i < (int)cur->subdirs.size(); i ++) {
			LayerNode *next = cur->subdirs[i];
			next->depth = cur->depth + 1;
			// generate subdir name in the current dir
			next->pwd = cur->pwd + "/" + generate_name(idx, cur->depth, cur->pwd.substr(prefix.length()), NODE_DIR);
			q.emplace_back(next);
		}
		delete cur;
	}

	std::string layer_name = prefix + "/layer" + std::to_string(idx);
	std::string cmd = std::string(" sudo tar  --xattrs --xattrs-include='*' -cf ") + layer_name + ".tar -C "  + prefix + " " + root_dirname;
	if (system(cmd.c_str()))
		LOG_ERROR_RETURN(-1, false, "fail to prepare tar file, cmd: `", cmd);
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
			img_file = LSMT::stack_files(current_layer, lowers, false, false);
		else
			img_file = current_layer;

		/* create erofs fs image */
		auto tar = new LibErofs(img_file, 4096, false);
		if (tar->extract_tar(src_file, true, i == 0)) {
			delete img_file;
			LOG_ERROR_RETURN(-1, nullptr, "fail to extract tar");
		}
		delete lowers;
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
			node = new StressNode(cur, NODE_DIR);
			auto dir = erofs_fs->opendir(cur.c_str());
			do {
				dirent *dent = dir->get();
				if (first)
					items.emplace_back(cur + std::string(dent->d_name));
				else
					items.emplace_back(cur + "/" + std::string(dent->d_name));
			} while (dir->next());
			dir->closedir();
			delete dir;
		} else if (S_ISREG(st.st_mode)) {
			photon::fs::IFile *file;
			bool ret;

			file = erofs_fs->open(cur.c_str(), O_RDONLY);
			node = new StressNode(cur, NODE_REGULAR);
			if (!file || ! node)
				LOG_ERROR_RETURN(0, false, "fail to open file or node `", cur);
			ret = verify_gen_mod(node, file) &&
				  verify_gen_own(node, file) &&
				  verify_gen_xattrs(node, file) &&
				  verify_gen_content(node, file);
			if (!ret)
				LOG_ERROR_RETURN(0, false, "fail to construct StressNode");
			file->close();
			delete file;
		}

		if (!tree->query_delete_node(node)) {
			delete node;
			LOG_ERROR_RETURN(-1, false, "file ` in erofs_fs but not in the in-mem tree", cur);
		}

		if (first)
			first = false;
	} while (!items.empty());

	return tree->is_emtry();
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

	std::string clear_cmd = std::string("rm -rf ") + prefix;
	if (system(clear_cmd.c_str()))
		LOG_ERROR_RETURN(-1, false, "fail to clear tmp workdir, cmd: `", clear_cmd);

	return ret;
}
