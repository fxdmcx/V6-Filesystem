#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

#define BLOCK_SIZE	512
#define INODE_SIZE	32

/* i-node flag bits */
#define INODE_ALLOC	0x8000		//indicate this i-node is allocated
#define IS_DIR		0x4000		//indicate associated file is directory
#define IS_LARGE	0x1000		//indicate associated file is a large file

#define ROOT_INUM	1		//I-node 1 is reserved for the root directory

static int initialized = 0;
static int block_num = 0;		//total number of blocks in the disk
static int inode_num = 0;		//total number of i-nodes in the disk
static unsigned short nfree = 0;
static unsigned short free_array[100];
static unsigned short ninode;
static unsigned short inode[100];
static int cur_dir_inum = 1;		//i-number represent current directory
//static char cur_path[128];
//static unsigned short inode_flags = 0;

struct super_block {
	unsigned short isize;		//number of blocks devoted to i-list
	unsigned short fsize;		//first block not potentially available for allocation
	unsigned short nfree;		
	unsigned short free[100];
	unsigned short ninode;
	unsigned short inode[100];
	char flock;
	char ilock;
	char fmod;
	unsigned short time[2];
} sp_blk;

/* i-nodes are 32 bytes long */
struct inode {
	unsigned short flags;
	char nlinks;
	char uid;
	char gid;
	char size0;
	unsigned short size1;
	unsigned short addr[8];
	unsigned short actime[2];
	unsigned short modtime[2];
};

/* directory entries are 16 bytes long */
struct dir_entry {
	unsigned short i_num;		//first word is i-number of the file
	char name[14];			//bytes 2-15 represent the file name
};


static void print_usage(void)
{
	printf("\n[v6 file system] following commands supported:\n"
		"=======================================================\n"
		"initfs block_num inode_num	//Initialize file system\n"
		"cpin externalfile v6-file	//copy external file into v6 fs\n"
		"				  create new file named v6-file in current directory\n"
		"cpout v6-file externalfile	//copy v6 file out to external file system\n"
		"mkdir v6-dir			//create v6-dir in current directory of v6 fs\n"
		"cd v6-dir			//access v6-dir in current directory of v6 fs\n"
		"rm v6-file			//delete v6-file if exists\n"
		"ls				//list all files exist in current directory\n"
		"q				//save chagnes and quit\n"
		"\n");
}

/* flush the standard input stream */
static void flush_std_input(void)
{
	char c;
	while ((c = getchar()) != '\n' && c != EOF) ;
}

/* update contents of the super block */
static void update_super_block(int fs_fd)
{
#if 0
	int i;
	printf("nfree = %d, ninode = %d\n", nfree, ninode);
	for (i = 0; i < nfree; i++)
		printf("%d ", free_array[i]);
	printf("\n");
	for (i = 0; i < 100; i++)
		printf("%d ", inode[i]);
	printf("\n");
#endif

	sp_blk.nfree = nfree;
	sp_blk.ninode = ninode;
	memcpy(sp_blk.free, free_array, 100 * sizeof(unsigned short));
	memcpy(sp_blk.inode, inode, 100 * sizeof(unsigned short));

	lseek(fs_fd, 1 * BLOCK_SIZE, SEEK_SET);
	write(fs_fd, &sp_blk, sizeof(sp_blk));
}

static void read_super_block(int fs_fd)
{
	lseek(fs_fd, 1 * BLOCK_SIZE, SEEK_SET);
	read(fs_fd, &sp_blk, sizeof(sp_blk));

	nfree = sp_blk.nfree;
	ninode = sp_blk.ninode;
	memcpy(free_array, sp_blk.free, 100 * sizeof(unsigned short));
	memcpy(inode, sp_blk.inode, 100 * sizeof(unsigned short));

#if 0
	int i;
	printf("nfree = %d, ninode = %d\n", nfree, ninode);
	for (i = 0; i < nfree; i++)
		printf("%d ", free_array[i]);
	printf("\n");
	for (i = 0; i < 100; i++)
		printf("%d ", inode[i]);
	printf("\n");
#endif
}

/*
 * add free block b into the free block list: set free_array[nfree] to the freed
 * block's number and increment nfree.(if nfree is 100, first copy nfree and the
 * free array into it, write it out, and set nfree to 0)
 */
static void add_free_block(int fs_fd, int b)
{
	if (nfree == 100) {
		lseek(fs_fd, b * BLOCK_SIZE, SEEK_SET);
		//printf("b = %d, sizeof(nfree)=%d, sizeof(free)=%d\n",
		//	b, sizeof(nfree), sizeof(free_array));
		write(fs_fd, &nfree, sizeof(nfree));
		write(fs_fd, free_array, sizeof(free_array));
		nfree = 0;
	}

	free_array[nfree++] = b;
}

/*
 * return a free block number: decrement nfree and return free_array[nfree] as
 * allocated block number. if nfree became 0, read in the block named by the new
 * block number, replace nfree by its first word, and copy the block numbers in
 * the next 100 words into the free array
 */
static int get_free_block(int fs_fd)
{
	int new_blk;

	nfree--;
	new_blk = free_array[nfree];

	/* if the new block number is 0, there are no blocks left */
	if (new_blk == 0) {
		fprintf(stderr, "Error: No blocks left!\n");
		return -1;
	}

	if (nfree == 0) {
		lseek(fs_fd, new_blk * BLOCK_SIZE, SEEK_SET);
		read(fs_fd, &nfree, sizeof(nfree));
		read(fs_fd, free_array, sizeof(free_array));
	}

	return new_blk;
}

/* when ninode equals 0, read the i-list and place the numbers of all free
 * inodes(up to 100) into the inode array
 */
static void reload_inode_array(int fs_fd)
{
	int i;
	int count = 0;
	struct inode nd;

	lseek(fs_fd, 2 * BLOCK_SIZE + 1 * INODE_SIZE, SEEK_SET);
	for (i = 2; i <= inode_num && count < 100; i++) {
		read(fs_fd, &nd, sizeof(nd));
		if ((nd.flags & INODE_ALLOC) == 0)
			inode[count++] = i;
	}
	ninode = count;
}

/* allocate an i-node and return the inode number */
static int get_free_inode(int fs_fd)
{
	if (ninode > 0)
		return inode[--ninode];
	else if (ninode == 0) {
		reload_inode_array(fs_fd);
		return get_free_inode(fs_fd);
	} else {
		fprintf(stderr, "No free inode could be allocated!\n");
		return -1;
	}
}

/* free I-node i and add it to the free inode array if ninode is less than 100 */
static void free_inode(int fs_fd, int i)
{
	struct inode nd;
	memset(&nd, 0, sizeof(nd));
	lseek(fs_fd, 2 * BLOCK_SIZE + (i-1) * INODE_SIZE, SEEK_SET);
	write(fs_fd, &nd, sizeof(nd));

	if (ninode < 100)
		inode[ninode++] = i;
}

/* find the file in v6 file system and return its associated i-node number */
static int locate_file(int fs_fd, char *file_name)
{	
	struct inode nd;
	struct dir_entry entry;
	int inum, blk_idx;
	int i, j;

	inum = cur_dir_inum;
	lseek(fs_fd, 2 * BLOCK_SIZE + (inum-1) * INODE_SIZE, SEEK_SET);
	read(fs_fd, &nd, sizeof(nd));

	for (i = 0; i < (nd.size1 / BLOCK_SIZE); i++) {
		blk_idx = nd.addr[i];
		lseek(fs_fd, blk_idx * BLOCK_SIZE, SEEK_SET);
		for (j = 0; j < BLOCK_SIZE / sizeof(entry); j++) {
			read(fs_fd, &entry, sizeof(entry));
			if (strcmp(entry.name, file_name) == 0)
				return entry.i_num;
		}
	}
	if ((nd.size1 % BLOCK_SIZE) != 0) {
		blk_idx = nd.addr[i];
		lseek(fs_fd, blk_idx * BLOCK_SIZE, SEEK_SET);
		for (j = 0; j < (nd.size1 % BLOCK_SIZE) / sizeof(entry); j++) {
			read(fs_fd, &entry, sizeof(entry));
			if (strcmp(entry.name, file_name) == 0)
				return entry.i_num;
		}
	}

	return -1;				//file not found, return -1
}

//static void add_dir_entry(struct dir_entry *entry) {}

/*
 * Initialize the V6 file system, there are block_num blocks and inode_num
 * inodes in the disk. The first block is left unused. The second block is used
 * as super block. I-nodes reside in the third and subsequent blocks
 */
static int init_v6fs(int fs_fd)
{
	unsigned int fs_size;
	int i;
	int cur_blk;
	int inode_block_num;

	if (initialized) {
		printf("v6 file system has been initialized already\n");
		return -1;
	}

	fs_size = BLOCK_SIZE * block_num;
	if (ftruncate(fs_fd, fs_size) < 0) {
		printf("Error: failed on setting the size of file system!\n");
		return -1;
	}

	inode_block_num = (inode_num + 15) / 16;	//16 i-nodes fit into a block
	cur_blk = 2 + inode_block_num;
	free_array[nfree++] = 0;			//initially set free_array[0] to 0

	/* set all data blocks to free */
	for (; cur_blk < block_num; cur_blk++)
		add_free_block(fs_fd, cur_blk);

	/* initialize all i-nodes data to 0 */
	lseek(fs_fd, 2 * BLOCK_SIZE, SEEK_SET);	
	char buf[INODE_SIZE] = {0};
	for (i = 0; i < inode_num; i++)
		write(fs_fd, buf, INODE_SIZE);

	/*
	 * initialize the root directory, i-node 1 is associated with root
	 * directory. Add the first two entries "." and ".." to root directory,
	 * where ".." has the same meaning as "."
	 */
	struct dir_entry entry1, entry2;
	entry1.i_num = ROOT_INUM;
	strcpy(entry1.name, ".");
	entry2.i_num = ROOT_INUM;
	strcpy(entry2.name, "..");

	cur_blk = get_free_block(fs_fd);
	lseek(fs_fd, cur_blk * BLOCK_SIZE, SEEK_SET);
	write(fs_fd, &entry1, sizeof(entry1));
	write(fs_fd, &entry2, sizeof(entry2));
	
	/* fill in contents of i-node 1 */
	struct inode nd;
	memset(&nd, 0, sizeof(nd));
	nd.flags = nd.flags | INODE_ALLOC | IS_DIR;
	//printf("nd.flags = 0x%x\n", nd.flags);
	nd.size1 = 2 * sizeof(entry1);
	nd.addr[0] = cur_blk;
	lseek(fs_fd, 2 * BLOCK_SIZE, SEEK_SET);
	write(fs_fd, &nd, sizeof(nd));

	/* initialize ninode and inode array */
	if ((inode_num - 1) < 100)
		ninode = inode_num - 1;
	else
		ninode = 100;
	for (i = 0; i < ninode; i++)
		inode[i] = 2 + i;			//free i-numbers start from 2

	/* initialize the super block */
	sp_blk.isize = inode_block_num;
	update_super_block(fs_fd);
	//read_super_block(fs_fd);

	initialized = 1;
	cur_dir_inum = ROOT_INUM;

	return 0;
}


static void cpin(int fs_fd, char *ext_file, char *v6_file)
{
	FILE *ext;
	unsigned int file_size;
	int req_blk_num;
	int i, blk_idx, inum;
	char buf[BLOCK_SIZE];
	struct inode nd;
	struct dir_entry entry;

	ext = fopen(ext_file, "r");
	if (ext == NULL) {
		fprintf(stderr, "open file %s failed!\n", ext_file);
		return;
	}

	inum = get_free_inode(fs_fd);
	if (inum < 0)
		return;
	memset(&nd, 0, sizeof(nd));
	nd.flags |= INODE_ALLOC;

	fseek(ext, 0, SEEK_END);
	file_size = ftell(ext);
	nd.size0 = file_size >> 16;
	nd.size1 = (unsigned short)file_size;
	//printf("nd.size0 = %d, nd.size1 = %d\n", nd.size0, nd.size1);
	req_blk_num = (file_size + BLOCK_SIZE - 1) / BLOCK_SIZE;
	if (req_blk_num <= 8) {				//small file
		fseek(ext, 0, SEEK_SET);
		for (i = 0; i < req_blk_num; i++) {
			fread(buf, 1, BLOCK_SIZE, ext);
			blk_idx = get_free_block(fs_fd);
			lseek(fs_fd, blk_idx * BLOCK_SIZE, SEEK_SET);
			write(fs_fd, buf, BLOCK_SIZE);
			nd.addr[i] = blk_idx;
		}
	} else {					//large file
		unsigned short indirect_block_data[256];
		int j;

		fseek(ext, 0, SEEK_SET);
		for (i = 0; i < req_blk_num / 256 && i < 7; i++) {
			blk_idx = get_free_block(fs_fd);
			nd.addr[i] = blk_idx;

			for (j = 0; j < 256; j++) {
				fread(buf, 1, BLOCK_SIZE, ext);
				blk_idx = get_free_block(fs_fd);
				indirect_block_data[j] = blk_idx;
				lseek(fs_fd, blk_idx * BLOCK_SIZE, SEEK_SET);
				write(fs_fd, buf, BLOCK_SIZE);
			}

			lseek(fs_fd, nd.addr[i] * BLOCK_SIZE, SEEK_SET);
			write(fs_fd, indirect_block_data, BLOCK_SIZE);
		}
		if (i == 7) {
			fprintf(stderr, "super large file, not supported!\n");
			return;
		}
		if ((req_blk_num % 256) != 0) {
			blk_idx = get_free_block(fs_fd);
			nd.addr[i] = blk_idx;
			for (j = 0; j < (req_blk_num % 256); j++) {
				fread(buf, 1, BLOCK_SIZE, ext);
				blk_idx = get_free_block(fs_fd);
				indirect_block_data[j] = blk_idx;
				lseek(fs_fd, blk_idx * BLOCK_SIZE, SEEK_SET);
				write(fs_fd, buf, BLOCK_SIZE);
			}
			lseek(fs_fd, nd.addr[i] * BLOCK_SIZE, SEEK_SET);
			write(fs_fd, indirect_block_data, (req_blk_num % 256) * 2);
		}

		nd.flags |= IS_LARGE;
	}

	lseek(fs_fd, 2 * BLOCK_SIZE + (inum-1) * INODE_SIZE, SEEK_SET);
	write(fs_fd, &nd, sizeof(nd));

	/* create corresponding directory entry */
	entry.i_num = inum;
	strcpy(entry.name, v6_file);
	//printf("entry.i_num is %d, entry.name is %s\n", entry.i_num, entry.name);
	lseek(fs_fd, 2 * BLOCK_SIZE + (cur_dir_inum-1) * INODE_SIZE, SEEK_SET);
	read(fs_fd, &nd, sizeof(nd));
	int entry_idx, block_idx;
	/* Let's suppose directory file is small file */
	if (nd.size1 % BLOCK_SIZE == 0) {
		block_idx = get_free_block(fs_fd);
		entry_idx = 0;
		nd.addr[nd.size1 / BLOCK_SIZE] = block_idx;
	} else {
		block_idx = nd.addr[nd.size1 / BLOCK_SIZE];
		entry_idx = (nd.size1 % BLOCK_SIZE) / sizeof(entry);
	}
	//printf("block_idx = %d, entry_idx = %d\n", block_idx, entry_idx);
	lseek(fs_fd, block_idx * BLOCK_SIZE + entry_idx * sizeof(entry), SEEK_SET);
	write(fs_fd, &entry, sizeof(entry));

	/* update contents of i-node representing current directory */
	nd.size1 += sizeof(entry);
	lseek(fs_fd, 2 * BLOCK_SIZE + (cur_dir_inum-1) * INODE_SIZE, SEEK_SET);
	write(fs_fd, &nd, sizeof(nd));

	printf("cpin command successfully executed, totally %d bytes copied\n", file_size);
	fclose(ext);
	return;
}

static void cpout(int fs_fd, char *v6_file, char *ext_file)
{
	FILE *ext;
	unsigned int file_size;
	int i, blk_idx, inum;
	char buf[BLOCK_SIZE];
	struct inode nd;
	//struct dir_entry entry;

	inum = locate_file(fs_fd, v6_file);
	//printf("cpout: the inum retrurned is %d\n", inum);
	if (inum < 0) {
		fprintf(stderr, "file %s does not exist in v6 file system, please check!\n", v6_file);
		return;
	}
	ext = fopen(ext_file, "w");
	if (ext == NULL) {
		fprintf(stderr, "open file %s failed!\n", ext_file);
		return;
	}

	lseek(fs_fd, 2 * BLOCK_SIZE + (inum-1) * INODE_SIZE, SEEK_SET);
	read(fs_fd, &nd, sizeof(nd));
	file_size = nd.size0 * (1 << 16) + nd.size1;
	//printf("nd.size0 = %d, nd.size1 = %d, file_size = %d\n", nd.size0, nd.size1, file_size);
	if ((nd.flags & IS_LARGE) == 0) {		//small file
		for (i = 0; i < (file_size / BLOCK_SIZE); i++) {
			blk_idx = nd.addr[i];
			lseek(fs_fd, blk_idx * BLOCK_SIZE, SEEK_SET);
			read(fs_fd, buf, BLOCK_SIZE);
			fwrite(buf, 1, BLOCK_SIZE, ext);
		}
		if ((file_size % BLOCK_SIZE) != 0) {
			blk_idx = nd.addr[i];
			lseek(fs_fd, blk_idx * BLOCK_SIZE, SEEK_SET);
			read(fs_fd, buf, file_size % BLOCK_SIZE);
			fwrite(buf, 1, file_size % BLOCK_SIZE, ext);
		}
	} else {
		int total_block, j;
		unsigned short indirect_block_data[256];
		total_block = (file_size + BLOCK_SIZE - 1) / BLOCK_SIZE;
		for (i = 0; i < (total_block - 1) / 256; i++) {
			blk_idx = nd.addr[i];
			lseek(fs_fd, blk_idx * BLOCK_SIZE, SEEK_SET);
			read(fs_fd, indirect_block_data, BLOCK_SIZE);
			for (j = 0; j < 256; j++) {
				blk_idx = indirect_block_data[j];
				lseek(fs_fd, blk_idx * BLOCK_SIZE, SEEK_SET);
				read(fs_fd, buf, BLOCK_SIZE);
				fwrite(buf, 1, BLOCK_SIZE, ext);
			}
		}
		if ((total_block - 1) % 256 != 0) {
			blk_idx = nd.addr[i];
			lseek(fs_fd, blk_idx * BLOCK_SIZE, SEEK_SET);
			read(fs_fd, indirect_block_data, BLOCK_SIZE);
			for (j = 0; j < (total_block - 1) % 256; j++) {
				blk_idx = indirect_block_data[j];
				lseek(fs_fd, blk_idx * BLOCK_SIZE, SEEK_SET);
				read(fs_fd, buf, BLOCK_SIZE);
				fwrite(buf, 1, BLOCK_SIZE, ext);
			}
			blk_idx = indirect_block_data[j];
			lseek(fs_fd, blk_idx * BLOCK_SIZE, SEEK_SET);
			read(fs_fd, buf, file_size % BLOCK_SIZE);
			fwrite(buf, 1, file_size % BLOCK_SIZE, ext);
		} else {
			blk_idx = nd.addr[i];
			lseek(fs_fd, blk_idx * BLOCK_SIZE, SEEK_SET);
			read(fs_fd, buf, file_size % BLOCK_SIZE);
			fwrite(buf, 1, file_size % BLOCK_SIZE, ext);
		}
	}

	printf("cpout command successfully executed, %d bytes written to file %s\n",
		file_size, ext_file);
	fclose(ext);
	return;
}

static void make_dir(int fs_fd, char *v6_dir)
{
	int inum, blk_idx;
	struct inode nd;
	struct dir_entry entry, entry1, entry2;

	if (locate_file(fs_fd, v6_dir) != -1) {
		printf("file with same name exists in current directory, "
			"please rename the directory file\n");
		return;
	}

	inum = get_free_inode(fs_fd);
	entry1.i_num = inum;
	strcpy(entry1.name, ".");
	entry2.i_num = cur_dir_inum;
	strcpy(entry2.name, "..");

	blk_idx = get_free_block(fs_fd);
	lseek(fs_fd, blk_idx * BLOCK_SIZE, SEEK_SET);
	write(fs_fd, &entry1, sizeof(entry1));
	write(fs_fd, &entry2, sizeof(entry2));

	memset(&nd, 0, sizeof(nd));
	nd.flags = INODE_ALLOC | IS_DIR;
	nd.size1 = 2 * sizeof(entry1);
	nd.addr[0] = blk_idx;
	lseek(fs_fd, 2 * BLOCK_SIZE + (inum-1) * INODE_SIZE, SEEK_SET);
	write(fs_fd, &nd, sizeof(nd));


	/* create corresponding entry in current directory */
	entry.i_num = inum;
	strcpy(entry.name, v6_dir);
	//printf("entry.i_num is %d, entry.name is %s\n", entry.i_num, entry.name);
	lseek(fs_fd, 2 * BLOCK_SIZE + (cur_dir_inum-1) * INODE_SIZE, SEEK_SET);
	read(fs_fd, &nd, sizeof(nd));
	int entry_idx, block_idx;
	if (nd.size1 % BLOCK_SIZE == 0) {
		block_idx = get_free_block(fs_fd);
		entry_idx = 0;
		nd.addr[nd.size1 / BLOCK_SIZE] = block_idx;
	} else {
		block_idx = nd.addr[nd.size1 / BLOCK_SIZE];
		entry_idx = (nd.size1 % BLOCK_SIZE) / sizeof(entry);
	}
	//printf("block_idx = %d, entry_idx = %d\n", block_idx, entry_idx);
	lseek(fs_fd, block_idx * BLOCK_SIZE + entry_idx * sizeof(entry), SEEK_SET);
	write(fs_fd, &entry, sizeof(entry));

	/* update contents of i-node representing current directory */
	nd.size1 += sizeof(entry);
	lseek(fs_fd, 2 * BLOCK_SIZE + (cur_dir_inum-1) * INODE_SIZE, SEEK_SET);
	write(fs_fd, &nd, sizeof(nd));


	return;
}


static void remove_file(int fs_fd, char *v6_file)
{
	int inum, blk_idx;
	int i, j, file_size; 
	struct inode nd;
	struct dir_entry entry;

	inum = locate_file(fs_fd, v6_file);
	if (inum < 0) {
		printf("file %s does not exist in current directory, please check!\n", v6_file);
		return;
	}


	lseek(fs_fd, 2 * BLOCK_SIZE + (inum-1) * INODE_SIZE, SEEK_SET);
	read(fs_fd, &nd, sizeof(nd));
	file_size = nd.size0 * (1 << 16) + nd.size1;
	//printf("nd.size0 = %d, nd.size1 = %d, file_size = %d\n", nd.size0, nd.size1, file_size);
	if ((nd.flags & IS_DIR) != 0) {
		printf("currently delete a directory not supported\n");
		return;
	}
	if ((nd.flags & IS_LARGE) == 0) {		//small file
		for (i = 0; i < (file_size / BLOCK_SIZE); i++) {
			blk_idx = nd.addr[i];
			add_free_block(fs_fd, blk_idx);
		}
		if ((file_size % BLOCK_SIZE) != 0) {
			blk_idx = nd.addr[i];
			add_free_block(fs_fd, blk_idx);
		}
	} else {					//large file
		int total_block, j;
		unsigned short indirect_block_data[256];
		total_block = (file_size + BLOCK_SIZE - 1) / BLOCK_SIZE;
		for (i = 0; i < total_block / 256; i++) {
			blk_idx = nd.addr[i];
			lseek(fs_fd, blk_idx * BLOCK_SIZE, SEEK_SET);
			read(fs_fd, indirect_block_data, BLOCK_SIZE);
			for (j = 0; j < 256; j++) {
				blk_idx = indirect_block_data[j];
				add_free_block(fs_fd, blk_idx);
			}
			add_free_block(fs_fd, nd.addr[i]);
		}
		if (total_block % 256 != 0) {
			blk_idx = nd.addr[i];
			lseek(fs_fd, blk_idx * BLOCK_SIZE, SEEK_SET);
			read(fs_fd, indirect_block_data, BLOCK_SIZE);
			for (j = 0; j < (total_block % 256); j++) {
				blk_idx = indirect_block_data[j];
				add_free_block(fs_fd, blk_idx);
			}
			add_free_block(fs_fd, nd.addr[i]);
		}
	}
	free_inode(fs_fd, inum);


	/* delete corresponding directory entry */
	lseek(fs_fd, 2 * BLOCK_SIZE + (cur_dir_inum-1) * INODE_SIZE, SEEK_SET);
	read(fs_fd, &nd, sizeof(nd));

	for (i = 0; i < (nd.size1 / BLOCK_SIZE); i++) {
		blk_idx = nd.addr[i];
		lseek(fs_fd, blk_idx * BLOCK_SIZE, SEEK_SET);
		for (j = 0; j < BLOCK_SIZE / sizeof(entry); j++) {
			read(fs_fd, &entry, sizeof(entry));
			if (strcmp(entry.name, v6_file) == 0)
				goto rm_entry;
		}
	}
	if ((nd.size1 % BLOCK_SIZE) != 0) {
		blk_idx = nd.addr[i];
		lseek(fs_fd, blk_idx * BLOCK_SIZE, SEEK_SET);
		for (j = 0; j < (nd.size1 % BLOCK_SIZE) / sizeof(entry); j++) {
			read(fs_fd, &entry, sizeof(entry));
			if (strcmp(entry.name, v6_file) == 0)
				goto rm_entry;
		}
	}

rm_entry:
	memset(&entry, 0, sizeof(entry));
	lseek(fs_fd, -1 * sizeof(entry), SEEK_CUR);
	write(fs_fd, &entry, sizeof(entry));

	//need update inode(like file_size) of current directory and move back
	//entries forward

	printf("command successfully executed, file %s has been deleted\n",v6_file);
	return;
}


static void access_dir(int fs_fd, char *v6_dir)
{
	int inum;
	char *dir_name;

	dir_name = strtok(v6_dir, "/");
	//printf("dir_name is %s\n", dir_name);
	inum = locate_file(fs_fd, dir_name);
	if (inum < 0) {
		printf("directory %s does not exist in current directory, please check!\n", v6_dir);
		return;
	}

	cur_dir_inum = inum;
}

/* check whether the file associated with inum is a directory file */
static int is_dir(int fs_fd, int inum)
{
	struct inode nd;

	lseek(fs_fd, 2 * BLOCK_SIZE + (inum-1) * INODE_SIZE, SEEK_SET);
	read(fs_fd, &nd, sizeof(nd));
	if ((nd.flags & IS_DIR) == 0)
		return 0;
	else
		return 1;
}

static void list_files(int fs_fd)
{
	int inum, blk_idx;
	int i, j;
	struct inode nd;
	struct dir_entry entry;

	inum = cur_dir_inum;
	lseek(fs_fd, 2 * BLOCK_SIZE + (inum-1) * INODE_SIZE, SEEK_SET);
	read(fs_fd, &nd, sizeof(nd));
	for (i = 0; i < (nd.size1 / BLOCK_SIZE); i++) {
		blk_idx = nd.addr[i];
		for (j = 0; j < BLOCK_SIZE / sizeof(entry); j++) {
			lseek(fs_fd, blk_idx * BLOCK_SIZE + j * sizeof(entry), SEEK_SET);
			read(fs_fd, &entry, sizeof(entry));
			if (entry.i_num == 0)
				continue;
			printf("%s", entry.name);
			if (is_dir(fs_fd, entry.i_num))
				printf("/");
			printf("    ");
		}
	}
	if ((nd.size1 % BLOCK_SIZE) != 0) {
		blk_idx = nd.addr[i];
		for (j = 0; j < (nd.size1 % BLOCK_SIZE) / sizeof(entry); j++) {
			lseek(fs_fd, blk_idx * BLOCK_SIZE + j * sizeof(entry), SEEK_SET);
			read(fs_fd, &entry, sizeof(entry));
			if (entry.i_num == 0)
				continue;
			printf("%s", entry.name);
			if (is_dir(fs_fd, entry.i_num))
				printf("/");
			printf("    ");
		}
	}

	printf("\n");
}


int main(int argc, char **argv)
{
	int fs_fd;
	char cmd[256];
	char *bin_cmd, *token;
	char *ext_file, *v6_file, *v6_dir;
	//int block_num, inode_num;

	if (argc < 2) {
		fprintf(stderr, "Invalid argument number: need a parameter "
			"to identify the path of file system image\n");
		exit(EXIT_FAILURE);
	}

	fs_fd = open(argv[1], O_RDWR|O_CREAT, S_IRUSR|S_IWUSR);
	if (fs_fd < 0) {
		fprintf(stderr, "Open file %s failed: %d, %s\n",
			argv[1], errno, strerror(errno));
		exit(EXIT_FAILURE);
	}

	print_usage();
	read_super_block(fs_fd);

	while (1) {
		printf("V6FS> ");
		cmd[0] = '\n';
		scanf("%[^\n]", cmd);
		/* in case user just typed \n */
		if (cmd[0] == '\n') {
			flush_std_input();
			continue;
		}
		//printf("The input command is %s\n", cmd);
		bin_cmd = strtok(cmd, " ");
		if (strcmp(bin_cmd, "initfs") == 0) {
			if ((token = strtok(NULL, " ")) == NULL) {
				fprintf(stderr, "Invalid parameter! should be: "
					"initfs block_num inode_num\n");
				flush_std_input();
				continue;
			} else {
				block_num = strtol(token, NULL, 0);
			}
			if ((token = strtok(NULL, " ")) == NULL) {
				fprintf(stderr, "Invalid parameter! should be: "
					"initfs block_num inode_num\n");
				flush_std_input();
				continue;
			} else {
				inode_num = strtol(token, NULL, 0);
			}

			//printf("block_num = %d, inode_num = %d\n", block_num, inode_num);
			init_v6fs(fs_fd);
		} else if (strcmp(bin_cmd, "cpin") == 0) {
			if ((token = strtok(NULL, " ")) == NULL) {
				fprintf(stderr, "Invalid parameter! should be: "
					"cpin externalfile v6-file\n");
				flush_std_input();
				continue;
			} else {
				ext_file = token;
			}
			if ((token = strtok(NULL, " ")) == NULL) {
				fprintf(stderr, "Invalid parameter! should be: "
					"cpin externalfile v6-file\n");
				flush_std_input();
				continue;
			} else {
				v6_file = token;
			}
			//printf("ext_file = %s, v6_file = %s\n", ext_file, v6_file);
			cpin(fs_fd, ext_file, v6_file);
		} else if (strcmp(bin_cmd, "cpout") == 0) {
			if ((token = strtok(NULL, " ")) == NULL) {
				fprintf(stderr, "Invalid parameter! should be: "
					"cpin externalfile v6-file\n");
				flush_std_input();
				continue;
			} else {
				v6_file = token;
			}
			if ((token = strtok(NULL, " ")) == NULL) {
				fprintf(stderr, "Invalid parameter! should be: "
					"cpin externalfile v6-file\n");
				flush_std_input();
				continue;
			} else {
				ext_file = token;
			}
			//printf("v6_file = %s, ext_file = %s\n", v6_file, ext_file);
			cpout(fs_fd, v6_file, ext_file);
		} else if (strcmp(bin_cmd, "mkdir") == 0) {
			if ((token = strtok(NULL, " ")) == NULL) {
				fprintf(stderr, "Invalid parameter! should be: "
					"mkdir v6-dir\n");
				flush_std_input();
				continue;
			} else {
				v6_dir = token;
			}
			//printf("v6_dir = %s\n", v6_dir);
			make_dir(fs_fd, v6_dir);
		} else if (strcmp(bin_cmd, "rm") == 0) {
			if ((token = strtok(NULL, " ")) == NULL) {
				fprintf(stderr, "Invalid parameter! should be: "
					"rm v6-file\n");
				flush_std_input();
				continue;
			} else {
				v6_file = token;
			}
			//printf("v6_file = %s\n", v6_file);
			remove_file(fs_fd, v6_file);
		} else if (strcmp(bin_cmd, "cd") == 0) {
			if ((token = strtok(NULL, " ")) == NULL) {
				fprintf(stderr, "Invalid parameter! should be: "
					"cd v6-dir\n");
				flush_std_input();
				continue;
			} else {
				v6_dir = token;
			}
			//printf("v6_file = %s\n", v6_file);
			access_dir(fs_fd, v6_dir);
		} else if (strcmp(bin_cmd, "ls") == 0) {
			list_files(fs_fd);
		} else if (strcmp(bin_cmd, "q") == 0) {
			update_super_block(fs_fd);
			printf("quit now!\n");
			exit(0);
		} else {
			printf("Invalid command!\n");
			flush_std_input();
			continue;
		}

		flush_std_input();
	}
}

