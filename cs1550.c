/*
FUSE: Filesystem in Userspace
Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>

This program can be distributed under the terms of the GNU GPL.
See the file COPYING.
*/

#define	FUSE_USE_VERSION 26

#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <assert.h>

//size of a disk block
#define	BLOCK_SIZE 512

//we'll use 8.3 filenames
#define	MAX_FILENAME 8
#define	MAX_EXTENSION 3

//How many files can there be in one directory?
#define MAX_FILES_IN_DIR (BLOCK_SIZE - sizeof(int)) / ((MAX_FILENAME + 1) + (MAX_EXTENSION + 1) + sizeof(size_t) + sizeof(long))

#define DISKSIZE_IN_BYTES 5242880

#define MAX_NUM_OF_BLOCKS DISKSIZE_IN_BYTES / BLOCK_SIZE

int filesystem_initialized = 0;

//The attribute packed means to not align these things
struct cs1550_directory_entry
{
	int nFiles;	//How many files are in this directory.
	//Needs to be less than MAX_FILES_IN_DIR

	struct cs1550_file_directory
	{
		char fname[MAX_FILENAME + 1];	//filename (plus space for nul)
		char fext[MAX_EXTENSION + 1];	//extension (plus space for nul)
		size_t fsize;					//file size
		long nStartBlock;				//where the first block is on disk
	} __attribute__((packed)) files[MAX_FILES_IN_DIR];	//There is an array of these

	//This is some space to get this to be exactly the size of the disk block.
	//Don't use it for anything.
	char padding[BLOCK_SIZE - MAX_FILES_IN_DIR * sizeof(struct cs1550_file_directory) - sizeof(int)];
} ;

typedef struct cs1550_root_directory cs1550_root_directory;

#define MAX_DIRS_IN_ROOT (BLOCK_SIZE - sizeof(int)) / ((MAX_FILENAME + 1) + sizeof(long))

struct cs1550_root_directory
{
	int nDirectories;	//How many subdirectories are in the root
	//Needs to be less than MAX_DIRS_IN_ROOT
	struct cs1550_directory
	{
		char dname[MAX_FILENAME + 1];	//directory name (plus space for nul)
		long nStartBlock;				//where the directory block is on disk
	} __attribute__((packed)) directories[MAX_DIRS_IN_ROOT];	//There is an array of these

	//This is some space to get this to be exactly the size of the disk block.
	//Don't use it for anything.
	char padding[BLOCK_SIZE - MAX_DIRS_IN_ROOT * sizeof(struct cs1550_directory) - sizeof(int)];
} ;


typedef struct cs1550_directory_entry cs1550_directory_entry;

//How much data can one block hold?
#define	MAX_DATA_IN_BLOCK (BLOCK_SIZE - sizeof(long))

struct cs1550_disk_block
{
	//The next disk block, if needed. This is the next pointer in the linked
	//allocation list
	long nNextBlock;

	//And all the rest of the space in the block can be used for actual data
	//storage.
	char data[MAX_DATA_IN_BLOCK];
};

typedef struct cs1550_disk_block cs1550_disk_block;

struct cs1550_free_space_tracker {
	char data[MAX_FILES_IN_DIR+1];
};

typedef struct cs1550_free_space_tracker cs1550_free_space_tracker;

static int check_fs_initialization();
static int initialize_filesystem();
static int find_unallocated_block(FILE *fs);
static void set_block_allocated(FILE *fs, int block_num);


/*
* Called whenever the system wants to know the file attributes, including
* simply whether the file exists or not.
*
* man -s 2 stat will show the fields of a stat structure
*/
static int cs1550_getattr(const char *path, struct stat *stbuf)
{
	if (check_fs_initialization() != 1) {
	initialize_filesystem();
	filesystem_initialized = 1;
}


char* diskfile = "./.disk";
FILE *fs = fopen(diskfile, "rb");
cs1550_root_directory *root_dir=malloc(sizeof(cs1550_root_directory));
if (fs == 0) {
	printf("cs1550_getattr(): could not open %s errno: %s\n", diskfile,strerror(errno));
} else {
	fseek(fs, 0, SEEK_SET); // make sure descriptor is at beginning of file
	if (fread(root_dir, sizeof(cs1550_root_directory), 1, fs) != 1) {
		printf("cs1550_getattr(): could not read root struct from %s errno: %s\n", diskfile,strerror(errno));
	}
}

int res = 0;
int i = 0;
/** These sizes are well above what is required
    to avoid fighting with overruns.
		Null termination is added appropriately later. **/
char extension[10];
char filename[10];
char directory[25];

sscanf(path,  "/%[^/]/%[^.].%s", directory, filename, extension);
//printf("cs1550_getattr(): Directory: %s Filename: %s Extension %s\n", directory, filename, extension);

memset(stbuf, 0, sizeof(struct stat));


int is_subdir = 1;
i = 0;
while (path[i] != NULL) {
	if (path[i] == '.') is_subdir = 0;
	i++;
}

//is path the root dir?
if (strcmp(path, "/") == 0) {
	stbuf->st_mode = S_IFDIR | 0755;
	stbuf->st_nlink = 2;
	printf("cs1550_getattr(): Setting stat structure for root directory.\n");
} else if (is_subdir) {
	/** Path denotes a directory. Does directory exist? **/
	int subdir_exists = 0;
	for (i=0; i<MAX_DIRS_IN_ROOT; i++) {
		if ( strncmp(root_dir->directories[i].dname, directory, 8) == 0 ) subdir_exists = 1;
	}

//	if (subdir_exists==1) printf("cs1550_getattr(): subdirectory %s exists.\n", path);
//	else printf("cs1550_getattr(): subdirectory %s does not exist.\n", directory);

	if (subdir_exists) {
		stbuf->st_mode = S_IFDIR | 0755;
		stbuf->st_nlink = 2;
		printf("cs1550_getattr(): Setting stat structure for subdirectory %s\n", directory);
		res = 0;
	} else res = -ENOENT;
}
else {
	printf("cs1550_getattr(): Getting attributes for file %s at path %s\n", filename, path);
	/** else it is a file. does file exist? **/
	int file_exists = 0;
	/** First look for its directory **/
	int subdir_exists = 0;
	int subdir_location_on_disk = 0;
	for (i=0; i<MAX_DIRS_IN_ROOT; i++) {
		if ( strncmp(root_dir->directories[i].dname, directory, 8) == 0 ) {
			subdir_exists = 1;
			subdir_location_on_disk = root_dir->directories[i].nStartBlock;
			break;
		}
	}
	/** Now look through the directory for the file. **/
	int file_size = 0;
	if (subdir_exists) {
		/** Get the directory entry from disk **/
		printf("cs1550_getattr(): Found subdirectory that the file is in..\n");
		cs1550_directory_entry *dir_entry = malloc(sizeof(cs1550_directory_entry));
		fseek(fs, subdir_location_on_disk*BLOCK_SIZE, SEEK_SET);
		if (fread(dir_entry, sizeof(cs1550_directory_entry), 1, fs) != 1) {
			printf("cs1550_getattr(): could not read directory entry struct from %s errno: %s\n", diskfile,strerror(errno));
		} else printf("cs1550_getattr(): loaded directory entry struct from block %i\n", subdir_location_on_disk);

		for (i=0; i<MAX_FILES_IN_DIR; i++) {
			if (dir_entry->files[i].fname != NULL) {
				printf("cs1550_getattr(): comparing %s and %s\n", dir_entry->files[i].fname, filename);
			}
			int f = strncmp(dir_entry->files[i].fname, filename, 8);
			int e = strncmp(dir_entry->files[i].fext, extension, 3);
			if ( (e==0) && (f==0) ) {
				file_exists = 1;
				file_size = dir_entry->files[i].fsize;
				break;
			}
		}
	}

	if (file_exists) {
		stbuf->st_mode = S_IFREG | 0666;
		stbuf->st_nlink = 1; //file links
		stbuf->st_size = file_size;
		res = 0;
		printf("cs1550_getattr(): Setting stat structure for file %s.%s\n", filename, extension);
	} else res = -ENOENT;
}

if (fs != NULL) fclose(fs);
return res;
}

/*
* Called whenever the contents of a directory are desired. Could be from an 'ls'
* or could even be when a user hits TAB to do autocompletion
*/
static int cs1550_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
	off_t offset, struct fuse_file_info *fi)
	{
		//Since we're building with -Wall (all warnings reported) we need
		//to "use" every parameter, so let's just cast them to void to
		//satisfy the compiler
		(void) offset;
		(void) fi;

		int r = 0;
		char extension[10];
		char filename[10];
		char directory[25];

		sscanf(path, "/%[^/]/%[^.].%s", directory, filename, extension);

		printf("cs1550_readdir(): attempting to list contents of %s\n", path);
		// Is path valid?
		if (strlen(path) > MAX_FILENAME+1) return -ENOENT;

		// Is path not a directory?
		int is_subdir = 1;
		int i = 0;
		while (path[i] != NULL) {
			if (path[i] == '.') is_subdir = 0;
			i++;
		}
		if (!is_subdir) return -ENOENT;

		// Check if directory exists
		char* disk_name = "./.disk";
		FILE *fs = fopen(disk_name, "rb");
		cs1550_root_directory *root_dir=malloc(sizeof(cs1550_root_directory));
		if (fs == 0) {
		r = -1;
		printf("cs1550_readdir(): could not open %s errno: %s\n", disk_name,strerror(errno));
	} else {
		if (fread(root_dir, sizeof(cs1550_root_directory), 1, fs) != 1) {
			r = -1;
			printf("cs1550_readdir(): could not read root struct from %s errno: %s\n", disk_name,strerror(errno));
		}
	}

	filler(buf, ".", NULL, 0);
	filler(buf, "..", NULL, 0);

	//If path is root directory, display subdirectories
	//If path is a subdirectory, display files

	if (strcmp(path, "/") == 0) {
		for(i=0;i<MAX_DIRS_IN_ROOT;i++) {
			if (root_dir->directories[i].dname != NULL) filler(buf, root_dir->directories[i].dname, NULL, 0);
		}
	} else {
		//Is a subdirectory. Does the subdirectory exist?
		int subdir_exists = 0;
		int subdir_location_on_disk = -1;
		for(i=0;i<MAX_DIRS_IN_ROOT;i++) {
			if (root_dir->directories[i].dname == NULL) continue;
			if ( strcmp(root_dir->directories[i].dname, directory) == 0 ) { subdir_exists = 1; subdir_location_on_disk = root_dir->directories[i].nStartBlock; }
		}
		if (!subdir_exists){ printf("cs1550_readdir(): could not find subdirectory %s\n", directory); return -ENOENT; }
		else {
			//List suddirectory's contents

			/** Get directory entry **/
			cs1550_directory_entry *dir_entry = malloc(sizeof(cs1550_directory_entry));
			fseek(fs, subdir_location_on_disk*BLOCK_SIZE, SEEK_SET);
			if (fread(dir_entry, sizeof(cs1550_directory_entry), 1, fs) != 1) {
				printf("cs1550_readdir(): could not read directory entry struct from %s errno: %s\n", disk_name,strerror(errno));
			}

			// List all files in directory
			char path_to_display[MAX_FILENAME + MAX_EXTENSION + 1 + 1];
			memset(path_to_display, 0, MAX_FILENAME + MAX_EXTENSION + 2);
			for(i=0;i<MAX_FILES_IN_DIR;i++) {
				if (dir_entry->files[i].fname[0] != NULL) {
					strncat(path_to_display, dir_entry->files[i].fname, 8);
					strncat(path_to_display, ".", 1);
					strncat(path_to_display, dir_entry->files[i].fext, 3);
				filler(buf, path_to_display, NULL, 0);
			}
			memset(path_to_display, 0, MAX_FILENAME + MAX_EXTENSION + 2);
			}

		}
	}

	if (fs != NULL) fclose(fs);
	return 0;
}

/*
* Creates a directory. We can ignore mode since we're not dealing with
* permissions, as long as getattr returns appropriate ones for us.
*/

static int cs1550_mkdir(const char *path, mode_t mode)
{
	(void) path;
	(void) mode;
	int w = 0;
	int r = 0;
	int err = 0;
	int i = 0;
	//char directory_name[strlen(path)+1]; // this doesn't work in sub C99, and may lead to bad buffer overruns
	char directory_name[MAX_FILENAME+1];

	if (check_fs_initialization() != 1) {
	initialize_filesystem();
	filesystem_initialized = 1;
}
	//	strcpy(directory_name, path+1); // +1 because of leading slash for root.
	strncpy(directory_name, path+1, MAX_FILENAME);
	directory_name[strlen(path)] = "\0";
	/** Check to see if we need to return an error
	*  The check to see if the directory already exists
	*  happens below after the root directory is read from disk. **/
	int seen_root=0;
	if (strlen(directory_name) > MAX_FILENAME+1) return -ENAMETOOLONG; // +1 because of root '/''

	for (i=0;i<strlen(path);i++) {
		if (path[i] == '/' && seen_root == 0) seen_root = 1;
		else if (path[i] == '/') return -EPERM;
	}

	/** END primary error checking **/
	char* filename = "./.disk";
	FILE *fs = fopen(filename, "rb+");
	cs1550_root_directory *root_dir=malloc(sizeof(cs1550_root_directory));
	cs1550_directory_entry *new_dir = malloc(sizeof(cs1550_directory_entry));
	assert(fs != 0);
	if (fs == 0) {
		err = errno;
		r = -1;
		assert(r==0);
		printf("cs1550_mkdir(): could not open %s errno: %s\n", filename,strerror(err));
	} else {
		/** Obtain root directory from disk **/
		w = fread(root_dir, sizeof(cs1550_root_directory), 1, fs);
		if ( w != 1) {
			err = errno;
			assert(r==0);
			r = -1;
			assert(r==0);
			printf("cs1550_mkdir(): could not read root struct from %s errno: %s\n", filename,strerror(err));
			fflush(stdout);
		}
		assert(r==0);
		/** Does directory already exist? **/
		for(i=0;i<MAX_DIRS_IN_ROOT;i++) {
			if ( strcmp(root_dir->directories[i].dname, directory_name) == 0 ) return -EEXIST;
		}
		/** Find somewhere to put the new directory **/
		int block_num = find_unallocated_block(fs);

		root_dir->nDirectories++;
		for(i=0;i<MAX_DIRS_IN_ROOT;i++) {
			if (root_dir->directories[i].dname[0] == NULL) {
				strncpy(root_dir->directories[i].dname, directory_name, 8);
				root_dir->directories[i].nStartBlock = block_num;
				break;
			}
		}

		/** Update root entry **/
		fseek(fs, 0, SEEK_SET);
		w = fwrite(root_dir, sizeof(cs1550_root_directory), 1, fs);
		if (w != 1) {
			printf("cs1550_mkdir(): fwrite() failed to update root directory on disk. errno: %s\n", strerror(errno));
			assert(r==0);
			r = -1;
			assert(r==0);
		}	else printf("cs1550_mkdir(): root directory successfully updated on disk.\n");
		/**/

		/** Set the new directory's block as allocated and write it to disk **/
		set_block_allocated(fs, block_num);
		assert(r==0);
		new_dir->nFiles = 0;
		for(i=0;i<MAX_FILES_IN_DIR;i++) new_dir->files[i].fname[0] = '\0'; // zero out all filenames in new directory
		assert(block_num != 0);
		fseek(fs, BLOCK_SIZE*block_num, SEEK_SET);
		printf("cs1550_mkdir(): writing new directory entry to byte position %i\n", BLOCK_SIZE*block_num);
		w = fwrite(new_dir, sizeof(cs1550_directory_entry), 1, fs);
		if (w != 1) {
			printf("cs1550_mkdir(): fwrite() failed to write new directory entry to disk. errno: %s\n", strerror(errno));
			assert(r==0);
			r = -1;
			assert(r==0);
		} else printf("cs1550_mkdir(): new directory entry successfully written to disk.\n");
		assert(r==0);


	}

	if (fs!=NULL) fclose(fs);
	return r;
}

static int find_unallocated_block(FILE *fs) {
	int r = 0;
	int unallocated_block = -1;
	cs1550_free_space_tracker *free_tracker =malloc(sizeof(cs1550_free_space_tracker));

	if (fs == NULL) printf("find_unallocated_block(): fs descriptor is NULL.\n");
	if (fseek(fs, DISKSIZE_IN_BYTES - BLOCK_SIZE, SEEK_SET) != 0) printf("find_unallocated_block() fseek() failed. errno: %s\n", strerror(errno));
	if (fread(free_tracker, sizeof(cs1550_free_space_tracker), 1, fs) != 1) {
		r = -1;
		printf("find_unallocated_block(): could not read free space tracker from disk errno: %s Location of stream: %i\n",strerror(errno), ftell(fs));
		if (ferror(fs)!=0) printf("ferror() returned nonzero\n");
	} else {
		// look for unallocated block
		int i;
		for (i=0; i<MAX_DATA_IN_BLOCK; i++) {
			if (free_tracker->data[i] == 0) {
				unallocated_block = i;
				break;
			}
		}
	}

	return unallocated_block;
}

static void set_block_allocated(FILE *fs, int block_num) {
	int r = 0;
	int w = -1;

	cs1550_free_space_tracker *free_tracker =malloc(sizeof(cs1550_free_space_tracker));
	fseek(fs, DISKSIZE_IN_BYTES - BLOCK_SIZE, SEEK_SET);
	if (fread(free_tracker, sizeof(cs1550_free_space_tracker), 1, fs) != 1) {
		r = -1;
		printf("set_block_allocated(): could not read free space tracker from disk errno: %s\n", strerror(errno));
	} else {
		// mark block allocated
		free_tracker->data[block_num] = 1;
		fseek(fs, DISKSIZE_IN_BYTES - BLOCK_SIZE, SEEK_SET);
		w = fwrite(free_tracker, sizeof(cs1550_free_space_tracker), 1, fs);
		if (w != 1) printf("set_block_allocated(): fwrite() failed to write free space tracker to disk. errno: %s\n", strerror(errno));
		else printf("set_block_allocated(): free space tracker updated.\n");
	}
}

/*
* Removes a directory.
*/
static int cs1550_rmdir(const char *path)
{
	(void) path;
	return 0;
}

static int initialize_filesystem() {
	int r = 0;
	int w = 0;
	char* filename = "./.disk";
	FILE *fs = fopen(filename, "rb+");
	if (fs == NULL) {
		int err = errno;
		r = 1;
		printf("initialize_filesystem(): could not open %s errno: %s\n", filename,strerror(err));
	} else {
		/** Create root directory **/
		cs1550_root_directory *root = malloc(sizeof(cs1550_root_directory));
		root->nDirectories = 0;
		int i;
		for (i=0;i<MAX_DIRS_IN_ROOT;i++) strcpy(root->directories[i].dname, "");
		fseek(fs, 0, SEEK_SET);
		w = fwrite(root, sizeof(cs1550_root_directory), 1, fs);
		if (w != 1) printf("initialize_filesystem(): fwrite() failed to write root directory to disk. errno: %s\n", strerror(errno));
		else printf("initialize_filesystem(): root directory initialized.\n");

		/** Create free space tracker **/
		cs1550_free_space_tracker *free_space = malloc(sizeof(cs1550_free_space_tracker));
		free_space->data[0] = 1; // show first block as allocated for root
		// need to mark as allocated the space used for the tracker!
		fseek(fs, DISKSIZE_IN_BYTES - BLOCK_SIZE, SEEK_SET);
		w = fwrite(free_space, sizeof(cs1550_free_space_tracker), 1, fs);
		if (w != 1) printf("initialize_filesystem(): fwrite() failed to write free space tracker to disk. errno: %s\n", strerror(errno));
		else printf("initialize_filesystem(): free space tracker initialized and written to byte position %i.\n", DISKSIZE_IN_BYTES - BLOCK_SIZE);
	}

	if (fs != NULL) fclose(fs);

	return r;
}

/*
* Does the actual creation of a file. Mode and dev can be ignored.
*
*/
static int cs1550_mknod(const char *path, mode_t mode, dev_t dev)
{
	(void) mode;
	(void) dev;

	/** These sizes are well above what is required
	    to avoid fighting with overruns.
			Null termination is added appropriately later. **/
	char extension[10];
	char filename[10];
	char directory[25];

	sscanf(path, "/%[^/]/%[^.].%s", directory, filename, extension);

	/** Check filename length **/
	if ( strnlen(filename, 9) > 8 || strnlen(extension, 4) > 3 ) {
		printf("cs1550_mknod(): filename or extension for %s too long.\n", path);
		return -ENAMETOOLONG;
	}
	/** Check if file creation is happening in root directory **/
	int in_root = 1;
	int i = 0;
	for(i=1;i<strlen(path);i++) if ( path[i] == '/' ) in_root = 0;
	if (in_root == 1) {
		printf("cs1550_mknod(): file at path %s being created in root directory.\n", path);
		return -EPERM;
	}

	/** Check if the file already exists
			If it doesn't, create it.    **/
	char* diskname = "./.disk";
	FILE *fs = fopen(diskname, "rb+");
	cs1550_root_directory *root_dir=malloc(sizeof(cs1550_root_directory));
	cs1550_directory_entry *dir = malloc(sizeof(cs1550_directory_entry));
	assert(fs != 0);
	if (fs == 0) {
		printf("cs1550_mknod(): could not open %s errno: %s\n", diskname,strerror(errno));
	} else {
		int dir_location = -1;
		if ( fread(root_dir, sizeof(cs1550_root_directory), 1, fs) != 1 ) printf("cs1550_mknod(): Could not read root directory from disk.\n");
		/** Find the directory that this file would be in **/
		for(i=0; i<MAX_DIRS_IN_ROOT; i++) {
			dir_location = root_dir->directories[i].nStartBlock;
			if ( strncmp(root_dir->directories[i].dname, directory, 8) == 0 ) break;
		}
		/** Directory that the file is in has been found **/
		fseek(fs, dir_location*BLOCK_SIZE, SEEK_SET);
		if ( fread(dir, sizeof(cs1550_directory_entry), 1, fs) != 1 ) printf("cs1550_mknod(): Could not read directory from disk.\n");
		int file_exists = 0;
		for(i=0; i<MAX_FILES_IN_DIR; i++) {
			if ( strncmp(filename, dir->files[i].fname, 8) == 0 && strncmp(extension, dir->files[i].fext, 3) == 0 ) return -EEXIST;
		}

		/** Directory has been searched, file has not been found.
				Create the file. **/
		int block_to_write = find_unallocated_block(fs);
		set_block_allocated(fs, block_to_write);
		/** Edit and write directory structure **/
		fseek(fs, dir_location*BLOCK_SIZE, SEEK_SET);
		dir->nFiles++;
		for(i=0;i<MAX_FILES_IN_DIR;i++) if (dir->files[i].fname[0] == NULL) break;
		strncpy(dir->files[i].fname, filename, 8);
		strncpy(dir->files[i].fext, extension, 3);
	//	dir->files[i].fname[MAX_FILENAME] = '\0';
	//	dir->files[i].fext[MAX_EXTENSION] = '\0';
		dir->files[i].fsize = 0;
		dir->files[i].nStartBlock = block_to_write;
		printf("cs1550_mknod(): updating directory entry with filename %s.%s to byte location %i\n", dir->files[i].fname, dir->files[i].fext, dir_location*BLOCK_SIZE);
		int w = fwrite(dir, sizeof(cs1550_directory_entry), 1, fs);
		if (w!=1) printf("cs1550_mknod(): fwrite failed to write updated directory entry to disk.\n");

		/** Create and write new file structure **/
		cs1550_disk_block *new_file=malloc(sizeof(cs1550_disk_block));
		memset(new_file->data, 5, MAX_DATA_IN_BLOCK);
		new_file->nNextBlock = -1;

		fseek(fs, block_to_write*BLOCK_SIZE, SEEK_SET);
		w = fwrite(new_file, sizeof(cs1550_disk_block), 1, fs);
		if (w!=1) printf("cs1550_mknod(): fwrite failed to write new file entry to disk.\n");
		else printf("cs1550_mknod(): Wrote new file entry to disk.\n");

	}

	if (fs!=NULL) fclose(fs);
	printf("cs1550_mknod(): Returning success from function.\n");
	return 0;
}

/*
* Deletes a file
*/
static int cs1550_unlink(const char *path)
{
	(void) path;

	return 0;
}

/*
* Read size bytes from file into buf starting from offset
*
*/
static int cs1550_read(const char *path, char *buf, size_t size, off_t offset,
	struct fuse_file_info *fi)
	{
		(void) buf;
		(void) offset;
		(void) fi;
		(void) path;

		//check to make sure path exists
		//check that size is > 0
		//check that offset is <= to the file size
		//read in data
		//set size and return, or error

		size = 0;

		return size;
	}

	/*
	* Write size bytes from buf into file starting from offset
	*
	*/
	static int cs1550_write(const char *path, const char *buf, size_t size,
		off_t offset, struct fuse_file_info *fi)
		{
			(void) buf;
			(void) offset;
			(void) fi;
			(void) path;

			//check to make sure path exists
			//check that size is > 0
			//check that offset is <= to the file size
			//write data
			//set size (should be same as input) and return, or error

			return size;
		}

		static int check_fs_initialization() {
			int r = 0;
			char* filename = "./.disk";
			FILE *fs = fopen(filename, "rb");
			cs1550_free_space_tracker *tracker = malloc(sizeof(cs1550_free_space_tracker));
			if (fs == 0) {
				r = -1;
				printf("check_fs_initialization(): could not open %s errno: %s\n", filename,strerror(errno));
			} else {
				fseek(fs, DISKSIZE_IN_BYTES - BLOCK_SIZE, SEEK_SET); //seek to free space tracker
				if (fread(tracker, sizeof(cs1550_free_space_tracker), 1, fs) != 1) {
					r = -1;
					printf("check_fs_initialization(): could not read root struct from %s errno: %s\n", filename,strerror(errno));
				} else {
					int init = 0;
					int i;
					for(i=0;i<MAX_FILES_IN_DIR;i++) if (tracker->data[i] == 1) init = 1;
					if (init) { r = 1; }
					else printf("check_fs_initialization(): Filesystem found to NOT be initialized.\n");
				}
			}

			if (fs != NULL) fclose(fs);
			return r;

		}

		/******************************************************************************
		*
		*  DO NOT MODIFY ANYTHING BELOW THIS LINE
		*
		*****************************************************************************/

		/*
		* truncate is called when a new file is created (with a 0 size) or when an
		* existing file is made shorter. We're not handling deleting files or
		* truncating existing ones, so all we need to do here is to initialize
		* the appropriate directory entry.
		*
		*/
		static int cs1550_truncate(const char *path, off_t size)
		{
			(void) path;
			(void) size;

			return 0;
		}


		/*
		* Called when we open a file
		*
		*/
		static int cs1550_open(const char *path, struct fuse_file_info *fi)
		{
			(void) path;
			(void) fi;
			/*
			//if we can't find the desired file, return an error
			return -ENOENT;
			*/

			//It's not really necessary for this project to anything in open

			/* We're not going to worry about permissions for this project, but
			if we were and we don't have them to the file we should return an error

			return -EACCES;
			*/

			return 0; //success!
		}

		/*
		* Called when close is called on a file descriptor, but because it might
		* have been dup'ed, this isn't a guarantee we won't ever need the file
		* again. For us, return success simply to avoid the unimplemented error
		* in the debug log.
		*/
		static int cs1550_flush (const char *path , struct fuse_file_info *fi)
		{
			(void) path;
			(void) fi;

			return 0; //success!
		}


		//register our new functions as the implementations of the syscalls
		static struct fuse_operations hello_oper = {
			.getattr	= cs1550_getattr,
			.readdir	= cs1550_readdir,
			.mkdir	= cs1550_mkdir,
			.rmdir = cs1550_rmdir,
			.read	= cs1550_read,
			.write	= cs1550_write,
			.mknod	= cs1550_mknod,
			.unlink = cs1550_unlink,
			.truncate = cs1550_truncate,
			.flush = cs1550_flush,
			.open	= cs1550_open,
		};

		//Don't change this.
		int main(int argc, char *argv[])
		{
			return fuse_main(argc, argv, &hello_oper, NULL);
		}
