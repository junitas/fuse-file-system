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
	char data[MAX_DATA_IN_BLOCK + 1];
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
	/*if (check_fs_initialization() != 0) {
		initialize_filesystem();
	}*/
	if (filesystem_initialized == 0) {
		initialize_filesystem();
	}

	char* diskfile = "./cs1550.disk";
	FILE *fs = fopen(diskfile, "rb");
	struct cs1550_root_directory;
	cs1550_root_directory *root_dir=malloc(sizeof(root_dir));
	if (fs == 0) {
		printf("cs1550_getattr(): could not open %s errno: %s\n", diskfile,strerror(errno));
	} else {
		fseek(fs, 0, SEEK_SET); // make sure descriptor is at beginning of file
		if (fread(root_dir, sizeof(root_dir), 1, fs) != 1) {
			printf("cs1550_getattr(): could not read root struct from %s errno: %s\n", diskfile,strerror(errno));
		}
	}
	printf("cs1550_getattr(): Extracted root node from .disk file. Number of subdirectories: %i\n", root_dir->nDirectories);

	int res = 0;
	int i = 0;
	char extension[10];
	char filename[10];
	char directory[25];

	sscanf(path, "/%s[^/]/%s[^.].%s", directory, filename, extension);
	printf("cs1550_getattr(): getting attributes of path: %s\n", path);
	printf("cs1550_getattr(): getting attributes of %s/%s.%s\n", directory, filename, extension);

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
	} else if (is_subdir) {
		/** Path denotes a directory. Does directory exist? **/
			int subdir_exists = 0;
				for (i=0; i<MAX_DIRS_IN_ROOT; i++) {
						if ( strcmp(root_dir->directories[i].dname, directory) == 0 ) subdir_exists = 1;
					}

			if (subdir_exists) printf("cs1550_getattr(): subdirectory %s exists.\n", path);
			else printf("cs1550_getattr(): subdirectory %s does not exist.\n", path);

			if (subdir_exists) {
				stbuf->st_mode = S_IFDIR | 0755;
				stbuf->st_nlink = 2;
				res = 0;
			} else res = -ENOENT;
		}
	else {
		/** else it is a file. does file exist? **/
		int file_exists = 0;
		/** First look for its directory **/
		int subdir_exists = 0;
		int subdir_location_on_disk = 0;
			for (i=0; i<MAX_DIRS_IN_ROOT; i++) {
					if ( strcmp(root_dir->directories[i].dname, directory) == 0 ) {
						subdir_exists = 1;
						subdir_location_on_disk = root_dir->directories[i].nStartBlock;
						break;
					}
				}
		/** Now look through the directory for the file. **/
		int file_size = 0;
		if (subdir_exists) {
			/** Get the directory entry from disk **/

			cs1550_directory_entry *dir_entry = malloc(sizeof(cs1550_directory_entry));
			fseek(fs, subdir_location_on_disk, SEEK_SET);
			if (fread(dir_entry, sizeof(root_dir), 1, fs) != 1) {
				printf("cs1550_getattr(): could not read directory entry struct from %s errno: %s\n", diskfile,strerror(errno));
			} else printf("cs1550_getattr(): loaded directory entry struct from block %i\n", subdir_location_on_disk);

			for (i=0; i<MAX_FILES_IN_DIR; i++) {
				int f = strcmp(dir_entry->files[i].fname, filename);
				int e = strcmp(dir_entry->files[i].fext, extension);
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

	//This line assumes we have no subdirectories, need to change
	if (strcmp(path, "/") != 0)
	return -ENOENT;

	//the filler function allows us to add entries to the listing
	//read the fuse.h file for a description (in the ../include dir)
	filler(buf, ".", NULL, 0);
	filler(buf, "..", NULL, 0);
	filler(buf, "testfiller", NULL, 0);
	printf("cs1550_readdir()...\n");

	/*
	//add the user stuff (subdirs or files)
	//the +1 skips the leading '/' on the filenames
	filler(buf, newpath + 1, NULL, 0);
	*/
	return 0;
}

/*
 * Creates a directory. We can ignore mode since we're not dealing with
 * permissions, as long as getattr returns appropriate ones for us.
 */
 /** JR NOTE: In this function we can check to see if the root
 * directory has been initialized. If not, we can initialize it and initialized
 *  the damn free space tracker. Because our root directory only contains
 *  subdirectories, users will call mkdir before creating any files. this
 *  is an appropriate time to initialize if not initialized **/
static int cs1550_mkdir(const char *path, mode_t mode)
{
	(void) path;
	(void) mode;
	int r, w = 0;
	int err = 0;
	int i = 0;
	char directory_name[strlen(path)];

	if (filesystem_initialized == 0) {
		initialize_filesystem();
	}

	strcpy(directory_name, path+1);
	/** Check to see if we need to return an error
	 *  The check to see if the directory already exists
	 *  happens below after the root directory is read from disk. **/
	int seen_root=0;
	if (strlen(directory_name) > 8) return -ENAMETOOLONG;

	for (i=0;i<strlen(path);i++) {
		if (path[i] == '/' && seen_root == 0) seen_root = 1;
		else if (path[i] == '/') return -EPERM;
	}


	/** END primary error checking **/
	char* filename = "./cs1550.disk";
	FILE *fs = fopen(filename, "rb+");
	cs1550_root_directory *root_dir=malloc(sizeof(cs1550_root_directory));
	cs1550_directory_entry *new_dir = malloc(sizeof(cs1550_directory_entry));
	if (fs == 0) {
		err = errno;
		r = -1;
		printf("cs1550_mkdir(): could not open %s errno: %s\n", filename,strerror(err));
	} else {
		/** Obtain root directory from disk **/
		if (fread(root_dir, sizeof(root_dir), 1, fs) != 1) {
			err = errno;
			r = -1;
			printf("cs1550_mkdir(): could not read root struct from %s errno: %s\n", filename,strerror(err));
		}

		/** Does directory already exist? **/
		for(i=0;i<MAX_DIRS_IN_ROOT;i++) {
			if ( strcmp(root_dir->directories[i].dname, directory_name) == 0 ) return -EEXIST;
		}
		/** Find somewhere to put the new directory **/
		int block_num = find_unallocated_block(fs);
		root_dir->nDirectories++;
		for(i=0;i<MAX_DIRS_IN_ROOT;i++) {
			if (root_dir->directories[i].dname[0] == NULL) {
				strcpy(root_dir->directories[i].dname,directory_name);
				root_dir->directories[i].nStartBlock = block_num;
				break;
			}
		}
		/** Set the new directory's block as allocated and write it to disk **/
		set_block_allocated(fs, block_num);
		new_dir->nFiles = 0;
		fseek(fs, BLOCK_SIZE*block_num, SEEK_SET);
		w = fwrite(new_dir, sizeof(cs1550_directory_entry), 1, fs);
		if (w != 1) { printf("cs1550_mkdir(): fwrite() failed to write new directory entry to disk. errno: %s\n", strerror(errno)); r = -1;}
		else printf("cs1550_mkdir(): new directory entry successfully written to disk.\n");

		/** Update root entry **/
		fseek(fs, 0, SEEK_SET);
		w = fwrite(root_dir, sizeof(cs1550_root_directory), 1, fs);
		if (w != 1) { printf("cs1550_mkdir(): fwrite() failed to update root directory on disk. errno: %s\n", strerror(errno)); r = -1;}
		else printf("cs1550_mkdir(): root directoy successfully updated on disk.\n");
		/**/

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

		printf("find_unallocated_block(): found unallocated block at block %i\n", unallocated_block);
		return unallocated_block;
}

static void set_block_allocated(FILE *fs, int block_num) {
	int r = 0;
	int w = -1;
	int unallocated_block = -1;

	cs1550_free_space_tracker *free_tracker =malloc(sizeof(cs1550_free_space_tracker));
		fseek(fs, DISKSIZE_IN_BYTES - BLOCK_SIZE, SEEK_SET);
		if (fread(free_tracker, sizeof(free_tracker), 1, fs) != 1) {
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
	char* filename = "./cs1550.disk";
	FILE *fs = fopen(filename, "rb+");
	if (fs == NULL) {
		int err = errno;
		r = 1;
		printf("initialize_filesystem(): could not open %s errno: %s\n", filename,strerror(err));
	} else {
		/** Create root directory **/
  	cs1550_root_directory *root = malloc(sizeof(cs1550_root_directory));
		root->nDirectories = 0;
		fseek(fs, 0, SEEK_SET);
		w = fwrite(root, sizeof(cs1550_root_directory), 1, fs);
		if (w != 1) printf("initialize_filesystem(): fwrite() failed to write root directory to disk. errno: %s\n", strerror(errno));
		else printf("initialize_filesystem(): root directory initialized.\n");

		/** Create free space tracker **/
		cs1550_free_space_tracker *free_space = malloc(sizeof(cs1550_free_space_tracker));
		free_space->data[0] = 1; // show first block as allocated for root
		fseek(fs, DISKSIZE_IN_BYTES - BLOCK_SIZE, SEEK_SET);
		w = fwrite(free_space, sizeof(cs1550_free_space_tracker), 1, fs);
		if (w != 1) printf("initialize_filesystem(): fwrite() failed to write free space tracker to disk. errno: %s\n", strerror(errno));
		else printf("initialize_filesystem(): free space tracker initialized.\n");
	}

	if (fs != NULL) fclose(fs);
	if (r == 0) filesystem_initialized = 1;
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
	int err;
	char* filename = "./cs1550.disk";
	FILE *fs = fopen(filename, "rb");
	struct cs1550_root_directory;
	cs1550_root_directory *root_dir=malloc(sizeof(root_dir));
	if (fs == 0) {
		err = errno;
		r = -1;
		printf("check_fs_initialization(): could not open %s errno: %s\n", filename,strerror(err));
	} else {
		if (fread(root_dir, sizeof(root_dir), 1, fs) != 1) {
			err = errno;
			r = -1;
			printf("check_fs_initialization(): could not read root struct from %s errno: %s\n", filename,strerror(err));
		} else {
			if (&root_dir == NULL) {
				r = -1; // i don't think this actually checks the struct correctly
				printf("check_fs_initialization(): root directory of filesystem is not initialized.\n");
		}
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
