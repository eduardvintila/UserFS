#include <linux/fs.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/err.h>

#define USERFS_MAGIC 0x13371337
#define USERFS_DIRS_OFFSET 1024
#define MAX_USERS 512
#define MAX_OUTPUT_SIZE 8192
#define COLOR_GREEN   "\x1b[32m"
#define COLOR_RESET   "\x1b[0m"

static long tura; /* TODO: de schimbat denumirea variabilei! */
static long ture[MAX_USERS]; // poate ar merge mai bine un malloc la userfs_init?
static int n;


/* Functie care creaza un nou inod si il initializeaza cu valori implicite */
struct inode *userfs_new_inode(struct super_block *sb, umode_t mode)
{
	struct inode *inode = new_inode(sb); /* Functie a sistemului de operare care aloca un inod */
	
	if (inode)
	{
		inode->i_mode = mode; /* Permisiuni */
		inode->i_blocks = 0; /* Numarul de blocuri(initial nu avem niciun bloc cu date) */
		inode->i_blkbits = inode->i_sb->s_blocksize_bits; /* Marimea unui block in biti(aceeasi ca a superblock-ului) */
		inode->i_mtime = inode->i_atime = inode->i_ctime = current_time(inode); /* Timpul trunchiat la granularitatea fs-ului */
// 		inode->i_uid = inode->i_gid = 0;
		inode->i_ino = get_next_ino(); /* Numarul inodului */
	}
	
	return inode;
}


/* Functia care se apeleaza la fiecare citire dintr-un fisier(procs) */ 
static ssize_t userfs_file_read (struct file *file, char *usrbuf, 
			size_t count, loff_t *offset)
{
	char *msg = kmalloc(MAX_OUTPUT_SIZE, GFP_KERNEL);
	char heading[] = "PID     Nume";
	int bytes = 0;
	struct task_struct *task;
	struct dentry *parent = file_dentry(file)->d_parent;
	long l_uid;
	kuid_t uid;
	long dif = MAX_OUTPUT_SIZE - *offset;
	//int procs = 0;
	ssize_t len = (count < dif) ? count : dif;
	
	if (len <= 0)
		return 0;
	
	if (kstrtol(parent->d_iname, 10, &l_uid) != 0)
		return 0; // EOF
	uid = KUIDT_INIT(l_uid);
	
	bytes += snprintf(msg, MAX_OUTPUT_SIZE, COLOR_GREEN "%s\n" COLOR_RESET, heading);
	
	rcu_read_lock();
	for_each_process(task)
	{
		kuid_t task_uid;
		
		task_lock(task);
		task_uid = task->cred->uid;
		if (uid_eq(uid, task_uid))
		{
			//procs++;
			if (bytes >= MAX_OUTPUT_SIZE)
			{
				task_unlock(task);
				break;
			}
			bytes += snprintf(msg + bytes, MAX_OUTPUT_SIZE - bytes, 
							  "%-7d %s\n", (int) task_pid_nr(task), task->comm);
		}
		task_unlock(task);
		
	}
	rcu_read_unlock();
		
	if (*offset > bytes)
		return 0;
		
	if (copy_to_user(usrbuf, msg + *offset, len) != 0)
		return -1; /* mai degraba errno.. */
	
	kfree(msg);
	
	*offset += len;
	return len;
}

/* Functia care se apeleaza la fiecare deschidere a unui fisier(procs) */
static int userfs_file_open (struct inode *inode, struct file *file)
{
	pr_info("USERFS: S-a deschis un fisier!\n");
	
	return 0;
}

void userfs_dentry_release(struct dentry *dentry)
{
	pr_info("UserFS: Se dezaloca dentry-ul: %s!\n", dentry->d_iname);
	if (dentry->d_inode)
	{
		pr_info("UserFS: Si inodul!\n");
		clear_inode(dentry->d_inode);
	}
}


/* Seteaza functiile ce vor fi apelate pentru diverse operatii I/O pe fisiere(procs) */
static const struct file_operations userfs_file_ops = 
{
		.open = userfs_file_open,
		.read = userfs_file_read
};

static const struct dentry_operations userfs_dentry_ops = 
{
		.d_release = userfs_dentry_release
};


/* Creeaza un fisier si asociaza-i o intrare de tip dentry.
 * Foarte asemanatoare cu crearea unui director(userfs_create_dir).
 */
int userfs_create_file(struct super_block *sb,
			struct dentry *parent, char *name, int len)
{
	struct inode *inode;
	struct dentry *dentry;
	struct qstr qname = QSTR_INIT(name, len);
	umode_t mode = S_IRUGO;
	
	dentry = d_hash_and_lookup(parent, &qname); // se mareste ref count
	if (dentry && dentry->d_inode)
	{
		dput(dentry);
		return 0;
	}
	
	dentry = d_alloc(parent, &qname); /* Aloca o intrare dentry */
	if (!dentry)
		goto err;
	
	dentry->d_op = &userfs_dentry_ops;
	inode = userfs_new_inode(sb, mode); /* Aloca un nou inod */
	if (!inode)
		goto err_free;
	
	inode->i_fop = &userfs_file_ops;
	
	d_add(dentry, inode); /* Adauga dentry-ul directorului creat in evidenta sistemului de operare */

	dput(dentry);
	return 0;
	
	err_free:
		dput(dentry);//d_drop(dentry); /* Elibereaza intrarea daca am intampinat o eroare */
	err:
		return -1;
}

/* Creeaza un director si asociaza-i o intrare de tip dentry.
 * Intrarile de tip dentry sunt folosite de kernel pentru parsarea cailor(path-urilor). 
 */
int userfs_create_dir(struct super_block *sb, struct dentry *parent,
			struct dir_context *ctx, char *name, int len)
{
	struct inode *inode;
	struct dentry *dentry;
	struct qstr qname = QSTR_INIT(name, len);
	umode_t mode = S_IFDIR | S_IXUGO | S_IRUGO; 
	
	dentry = d_hash_and_lookup(parent, &qname); // se mareste ref count
	if (dentry && dentry->d_inode)
	{
		int *fsdata = dentry->d_fsdata;
		
		if (*fsdata == tura) // am afisat deja directorul
		{
			//pr_info("Am gasit deja tura asta: nr %ld, nume: %s\n", tura, name);
			dput(dentry);
			return 0;
		}
		
		//pr_info("Prima oara pe tura asta: nr %ld, nume: %s\n", tura, name);
		*fsdata = tura; // marcheaza ca am afisat tura asta
		inode = dentry->d_inode;
		dput(dentry);
		ctx->pos += 1;
		return dir_emit(ctx, name, len, inode->i_ino, inode->i_mode >> 12); 
	}
	if (!dentry) // daca nu intra pe ramura asta, inseamna ca dentryul nu are un inod asociat
	{
		//pr_info("Nu am gasit %s\n", name);
		dentry = d_alloc(parent, &qname); /* Aloca o intrare dentry */
		if (!dentry)
			goto err;
	}	
	inode = userfs_new_inode(sb, mode); /* Aloca un nou inod */
	if (!inode)
		goto err_free;
	
	ture[n] = tura; // atentie la conditii de cursa....
	dentry->d_fsdata = &ture[n];
	n = (n + 1) % MAX_USERS;
	
	inode->i_fop = &simple_dir_operations; 
	inode->i_op  = &simple_dir_inode_operations; 
	d_add(dentry, inode); /* Adauga dentry-ul directorului creat in evidenta sistemului de operare */
	
	if (userfs_create_file(sb, dentry, "procs", 5) != 0)
		goto err_free;
		
	dput(dentry);
	ctx->pos += 1;
	return dir_emit(ctx, name, len, inode->i_ino, inode->i_mode >> 12); // fill info
	
	err_free:
		dput(dentry);//d_drop(dentry); /* Elibereaza intrarea daca am intampinat o eroare */
	err:
		return -1;
}


/* Creeaza directoarele pentru fiecare user si cate un fisier procs corespunzator */
static int userfs_root_readdir(struct file *file, struct dir_context *ctx)
{
	/* TODO: ATENTIE LA CONDITIILE DE CURSA! */
	/* TODO: MOMENTAN MERGE, DAR E CAM INEFICIENT(SE ALOCA MEMORIE IN PROSTIE!), DE MEMORY LEAK-URI NICI NU MAI VORBIM..*/
	/* TODO: STERGEREA DIRECTOARELOR PT USERI INACTIVI(oare ar trebui sa eliberez toate dentry-urile, asemanator cu proc?) */
	
	struct super_block *sb = file_inode(file)->i_sb;
	struct dentry *root = sb->s_root;
	struct task_struct *task;
	kuid_t uid;
	
	if (ctx->pos > USERFS_DIRS_OFFSET)
		return 0; /* nu mai avem ce citi */
		
	if (!dir_emit_dots(file, ctx)) /* trecem peste '.' si '..' */
		return 0;
	
	rcu_read_lock();
	// preia lacat si pentru tura!
	for_each_process(task)
	{
// 		struct dentry *dentry;
// 		struct dentry *dentry_procs;
// 		struct inode  *inode;
		char name[20]; /* TODO:  de schimbat aici */
		int len;
		
		task_lock(task);
		uid = task->cred->uid; /* Preia id-ul utilizatorului */
		task_unlock(task);
		
		len = snprintf(name, sizeof(name), "%u", uid.val);
		
		userfs_create_dir(sb, root, ctx, name, len); // de verificat si aici eroare
		
		/* TODO: de verificat daca bufferul userului e plin */
		
	}
	rcu_read_unlock();
	// cedeaza lacatul si pentru tura!
	
	ctx->pos = USERFS_DIRS_OFFSET + 1;
	tura++;
	
	return 0;
}

static struct dentry *userfs_root_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags)
{
	struct task_struct *task;
	const unsigned char *name = dentry->d_name.name;
	long   l_uid;
	kuid_t uid;
	
	if (kstrtol(name, 10, &l_uid) != 0)
		goto not_found;
	uid = KUIDT_INIT(l_uid);
	
	if (dentry->d_inode)
	{
		pr_info("Lookup %s: valid dentry.\n", name);
	}
	else
		pr_info("Lookup %s: invalid dentry.\n", name);
	
	//return simple_lookup(dir, dentry, flags);
	
	rcu_read_lock();
	for_each_process(task)
	{
		kuid_t task_uid;
		
		task_lock(task);
		task_uid = task->cred->uid; /* Preia id-ul utilizatorului */
		task_unlock(task);
		
		if (uid_eq(uid, task_uid))
		{
			umode_t mode = S_IFDIR | S_IXUGO | S_IRUGO;
			struct inode *inode = userfs_new_inode(dir->i_sb, mode);
			
			if (!inode)
			{
				pr_err("Eroare alocare inod lookup\n");
				return ERR_PTR(-1);
			}
			
			inode->i_fop = &simple_dir_operations;
			inode->i_op = &simple_dir_inode_operations;
			dentry->d_fsdata = &ture[n];
			n = (n + 1) % MAX_USERS;
			d_add(dentry, inode);
			
			userfs_create_file(dir->i_sb, dentry, "procs", 5);
			
			// ceva cu icount?
			rcu_read_unlock();
			return NULL;
		}
		
	}
	rcu_read_unlock();
	
not_found:
	d_add(dentry, NULL);
	return NULL;
}


const struct super_operations userfs_sb_ops =
{
	.statfs    = simple_statfs,         /* Output pentru comenzile 'stat -f' sau 'df' */
	.drop_inode = generic_delete_inode  /* Se apeleaza la stergerea unui inod */
	
	/* TODO: show_options */
};

static const struct file_operations userfs_root_ops =
{
	.read = generic_read_dir,
	.iterate = userfs_root_readdir,
	.llseek = generic_file_llseek
};

static const struct inode_operations userfs_root_inode_ops = 
{
	.lookup = userfs_root_lookup
};


/* Functie care initializeaza superblock-ul sistemului de fisiere */
int userfs_fill_sb(struct super_block *sb, void *data, int silent)
{
	struct inode *inode;
	
	sb->s_blocksize      = PAGE_SIZE;      /* Marimea block-ului in bytes*/
	sb->s_blocksize_bits = PAGE_SHIFT;     /* Marimea block-ului in biti */
	sb->s_time_gran      = 1;              /* Time granularity(precizie?) */
	sb->s_op             = &userfs_sb_ops; /* Operatii pe superblock */
	sb->s_magic          = USERFS_MAGIC;   /* Indentificatorul sistemului de fisiere */
	
	inode = userfs_new_inode(sb, S_IFDIR | S_IXUGO | S_IRUGO); /* Creeaza un nou inod. */
	inode->i_fop = &userfs_root_ops;
	inode->i_op = &userfs_root_inode_ops;
	sb->s_root = d_make_root(inode);                 /* Fa-l radacina. */
	
	if (!sb->s_root)
	{
		pr_err("Eroare alocare inod!\n");
// 		return -ENOMEM; /* Out of memory */
		return -1;
	}

	return 0;
}



/* Functie care intoarce un pointer catre directorul radacina */
struct dentry *userfs_mount(struct file_system_type *fs_type,
		int flags, const char *dev_name, void *data)
{
	/* mount_nodev este o functie a sistemului de operare care monteaza un
	 * sistem de fisiere ce nu are la baza un disk fizic. Parametrul userfs_fill_sb
	 * este un pointer catre functia care va incarca(initializa) superblock-ul sistemului de fisiere.
	 */
	
	return mount_nodev(fs_type, flags, data, userfs_fill_sb);
}


/* Structura care defineste sistemul de fisiere */
static struct file_system_type userfs_type =
{
	.name = "userfs", /* Numele sistemului de fisiere care se va folosi la montarea cu 'mount -t' */
	.owner = THIS_MODULE,
	.mount = userfs_mount, /* Functia care se va apela la montare */
	.kill_sb = kill_anon_super /* Functie a sistemului de operare care se va apela la demontare
									pentru a elibera structurile interne. */
};

static int userfs_init(void)
{
	/* Inregistreaza sistemul de fisiere in evidenta sistemului de operare */
	
	if (register_filesystem(&userfs_type) != 0)
	{
		pr_err("Eroare la inregistrarea UserFS!\n");
// 		return errno;
		return -1;
	}
	
	pr_info("Inregistrare UserFS - Succes!\n");
	return 0;
}

static void userfs_exit(void)
{
	/* Scoate sistemul de fisiere din evidenta sistemului de operare */
	
	if (unregister_filesystem(&userfs_type) != 0)
		pr_err("Eroare la deinregistrare\n"); //TODO: De schimbat mesajul!
	else
		pr_info("Deinregistrare UserFS - Succes!\n");
	
}


module_init(userfs_init);
module_exit(userfs_exit);

MODULE_LICENSE("GPL"); /* De schimbat! */
MODULE_DESCRIPTION("UserFS");
MODULE_AUTHOR("Eduard Ionut Vintila");

