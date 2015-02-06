// Simple command-line kernel monitor useful for
// controlling the kernel and exploring the system interactively.

#include <inc/stdio.h>
#include <inc/string.h>
#include <inc/memlayout.h>
#include <inc/assert.h>
#include <inc/x86.h>

#include <kern/console.h>
#include <kern/monitor.h>
#include <kern/kdebug.h>
#include <kern/pmap.h>

#define CMDBUF_SIZE	80	// enough for one VGA text line


struct Command {
	const char *name;
	const char *desc;
	// return -1 to force monitor to exit
	int (*func)(int argc, char** argv, struct Trapframe* tf);
};

static struct Command commands[] = {
	{ "help", "Display this list of commands", mon_help },
	{ "kerninfo", "Display information about the kernel", mon_kerninfo },
	{ "backtrace", "Display a backtrace", mon_backtrace },
	{ "showmappings", "Show physical page mappings", mon_showmappings },
	{ "perm", "Adjust permissions on a page.", mon_perm }
};
#define NCOMMANDS (sizeof(commands)/sizeof(commands[0]))

/***** Implementations of basic kernel monitor commands *****/

int
mon_help(int argc, char **argv, struct Trapframe *tf)
{
	int i;

	for (i = 0; i < NCOMMANDS; i++)
		cprintf("%s - %s\n", commands[i].name, commands[i].desc);
	return 0;
}

int
mon_kerninfo(int argc, char **argv, struct Trapframe *tf)
{
	extern char _start[], entry[], etext[], edata[], end[];

	cprintf("Special kernel symbols:\n");
	cprintf("  _start                  %08x (phys)\n", _start);
	cprintf("  entry  %08x (virt)  %08x (phys)\n", entry, entry - KERNBASE);
	cprintf("  etext  %08x (virt)  %08x (phys)\n", etext, etext - KERNBASE);
	cprintf("  edata  %08x (virt)  %08x (phys)\n", edata, edata - KERNBASE);
	cprintf("  end    %08x (virt)  %08x (phys)\n", end, end - KERNBASE);
	cprintf("Kernel executable memory footprint: %dKB\n",
		ROUNDUP(end - entry, 1024) / 1024);
	return 0;
}

int
mon_backtrace(int argc, char **argv, struct Trapframe *tf)
{
	cprintf("Stack backtrace:\n");
	uint32_t *ebp = (uint32_t *) read_ebp();
	while (ebp) {
		uint32_t eip = ebp[1];
		uint32_t *args = &ebp[2];
		int i = 0;

		cprintf("  ebp %08x  eip %08x  args %08x %08x %08x %08x %08x\n",
				ebp, eip,
				args[0], args[1], args[2], args[3], args[4]);

		struct Eipdebuginfo info;
		debuginfo_eip(eip, &info);
		cprintf("         %s:%d: %.*s+%d\n",
				info.eip_file, info.eip_line,
				info.eip_fn_namelen, info.eip_fn_name,
				eip - info.eip_fn_addr);
		ebp = (uint32_t *) *ebp;
	}
	return 0;
}

// Given a string containing a hexadecimal address like "0xdeadbeef",
// parseaddr parses it into a number and places it in the out
// parameter.  Returns 0 on success and -1 on error.
static int
parseaddr(const char *argv, uintptr_t *out)
{
	static char digits[] = "0123456789abcdef";

	if (*argv++ != '0' || *argv++ != 'x')
		return -1;
	uintptr_t n = 1, res = 0;
	const char *cur = argv + strlen(argv);
	if (cur - argv > 8)
		return -1;
	while (cur-- > argv) {
		char *needle = strchr(digits, *cur);
		if (!needle && *cur >= 'A' && *cur <= 'F')
			needle = strchr(digits, *cur + 0x20);
		else if (!needle)
			return -1;
		res += (n * (needle - digits));
		n *= 16;
	}
	*out = res;
	return 0;
}

static void
print_perms(pte_t pte)
{
	if (pte & PTE_W)
		cprintf("W");
	if (pte & PTE_U)
		cprintf("U");
}

int
mon_showmappings(int argc, char **argv, struct Trapframe *tf)
{
	if (argc != 3) {
		cprintf("usage: %s START END\n", argv[0]);
		return 0;
	}
	uintptr_t addrs[2];
	size_t i;
	for (i = 0; i < 2; i++) {
		if (parseaddr(argv[i+1], &addrs[i]) == -1) {
			cprintf("error: couldn't parse '%s' as address.  ex: 0x1000\n", argv[i]);
			return 0;
		}
		if (addrs[i] % PGSIZE) {
			cprintf("error: 0x%08x must be page aligned.\n", addrs[i]);
			return 0;
		}
	}
	pde_t *pgdir = (pde_t *) (KERNBASE + rcr3());
	cprintf("Mappings on virtual addresses [0x%08x, 0x%08x):\n", addrs[0], addrs[1]);
	// Hint: to get the mapping for 0xfffff000, use `showmappings 0xfffff000 0x0`.
	for (i = addrs[0]; i != addrs[1]; i += PGSIZE) {
		cprintf("  0x%08x ", i);
		pte_t *pte = pgdir_walk(pgdir, (const void *) i, false);
		if (pte && (*pte & PTE_P)) {
			physaddr_t paddr = PTE_ADDR(*pte);
			cprintf("-> 0x%08x ", paddr);
		} else {
			cprintf("(unmapped)\n");
			continue;
		}
		print_perms(*pte);
		cprintf("\n");
	}
	return 0;
}

// Returns '|' to "OR" the PTE with the value placed into out.  Returns
// '&' to "AND" with the value placed into out.  Returns 0 on parse
// error.
static char
parse_perm_adjust(const char *arg, pte_t *out)
{
	char oper = 0;
	pte_t mask = 0;
	switch (*arg) {
	case '+':
		oper = '|';
		break;
	case '-':
		oper = '&';
		break;
	default:
		return 0;
	}
	++arg;
	if (!arg[0] || arg[1])
		return 0;
	switch (*arg) {
	case 'W':
		mask |= PTE_W;
		break;
	case 'U':
		mask |= PTE_U;
		break;
	default:
		return 0;
	}
	if (oper == '&')
		mask ^= ~0x0;
	*out = mask;
	return oper;
}

int
mon_perm(int argc, char **argv, struct Trapframe *tf)
{
	uintptr_t addr;
	pte_t mask;
	char adjust;
	if (argc != 3 || parseaddr(argv[1], &addr) || !(adjust = parse_perm_adjust(argv[2], &mask))) {
		cprintf("usage: %s ADDR [+-][WU]\n", argv[0]);
		return 0;
	}
	pde_t *pgdir = (pde_t *) (KERNBASE + rcr3());
	pte_t *pte = pgdir_walk(pgdir, (void *) addr, false);
	if (!pte || !(*pte & PTE_P)) {
		cprintf("0x%08x is unmapped.\n");
		return 0;
	}
	switch (adjust) {
	case '&':
		*pte &= mask;
		break;
	case '|':
		*pte |= mask;
		break;
	default:
		panic("shouldn't happen");
		return 0;
	}
	cprintf("Set permissions on 0x%08x to: ");
	print_perms(*pte);
	cprintf("\n");
	return 0;
}

/***** Kernel monitor command interpreter *****/

#define WHITESPACE "\t\r\n "
#define MAXARGS 16

static int
runcmd(char *buf, struct Trapframe *tf)
{
	int argc;
	char *argv[MAXARGS];
	int i;

	// Parse the command buffer into whitespace-separated arguments
	argc = 0;
	argv[argc] = 0;
	while (1) {
		// gobble whitespace
		while (*buf && strchr(WHITESPACE, *buf))
			*buf++ = 0;
		if (*buf == 0)
			break;

		// save and scan past next arg
		if (argc == MAXARGS-1) {
			cprintf("Too many arguments (max %d)\n", MAXARGS);
			return 0;
		}
		argv[argc++] = buf;
		while (*buf && !strchr(WHITESPACE, *buf))
			buf++;
	}
	argv[argc] = 0;

	// Lookup and invoke the command
	if (argc == 0)
		return 0;
	for (i = 0; i < NCOMMANDS; i++) {
		if (strcmp(argv[0], commands[i].name) == 0)
			return commands[i].func(argc, argv, tf);
	}
	cprintf("Unknown command '%s'\n", argv[0]);
	return 0;
}

void
monitor(struct Trapframe *tf)
{
	char *buf;

	cprintf("Welcome to the JOS kernel monitor!\n");
	cprintf("Type 'help' for a list of commands.\n");

	while (1) {
		buf = readline("K> ");
		if (buf != NULL)
			if (runcmd(buf, tf) < 0)
				break;
	}
}
