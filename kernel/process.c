/*
 * Utility functions for process management. 
 *
 * Note: in Lab1, only one process (i.e., our user application) exists. Therefore, 
 * PKE OS at this stage will set "current" to the loaded user application, and also
 * switch to the old "current" process after trap handling.
 */

#include "riscv.h"
#include "strap.h"
#include "config.h"
#include "process.h"
#include "elf.h"
#include "string.h"
#include "vmm.h"
#include "pmm.h"
#include "memlayout.h"
#include "spike_interface/spike_utils.h"

//Two functions defined in kernel/usertrap.S
extern char smode_trap_vector[];
extern void return_to_user(trapframe *, uint64 satp);

// current points to the currently running user-mode application.
process* current = NULL;

// points to the first free page in our simple heap. added @lab2_2
uint64 g_ufree_page = USER_FREE_ADDRESS_START;

//
// switch to a user-mode process
//
void switch_to(process* proc) {
  assert(proc);
  current = proc;

  // write the smode_trap_vector (64-bit func. address) defined in kernel/strap_vector.S
  // to the stvec privilege register, such that trap handler pointed by smode_trap_vector
  // will be triggered when an interrupt occurs in S mode.
  write_csr(stvec, (uint64)smode_trap_vector);

  // set up trapframe values (in process structure) that smode_trap_vector will need when
  // the process next re-enters the kernel.
  proc->trapframe->kernel_sp = proc->kstack;      // process's kernel stack
  proc->trapframe->kernel_satp = read_csr(satp);  // kernel page table
  proc->trapframe->kernel_trap = (uint64)smode_trap_handler;

  // SSTATUS_SPP and SSTATUS_SPIE are defined in kernel/riscv.h
  // set S Previous Privilege mode (the SSTATUS_SPP bit in sstatus register) to User mode.
  unsigned long x = read_csr(sstatus);
  x &= ~SSTATUS_SPP;  // clear SPP to 0 for user mode
  x |= SSTATUS_SPIE;  // enable interrupts in user mode

  // write x back to 'sstatus' register to enable interrupts, and sret destination mode.
  write_csr(sstatus, x);

  // set S Exception Program Counter (sepc register) to the elf entry pc.
  write_csr(sepc, proc->trapframe->epc);

  // make user page table. macro MAKE_SATP is defined in kernel/riscv.h. added @lab2_1
  uint64 user_satp = MAKE_SATP(proc->pagetable);

  // return_to_user() is defined in kernel/strap_vector.S. switch to user mode with sret.
  // note, return_to_user takes two parameters @ and after lab2_1.
  return_to_user(proc->trapframe, user_satp);
}

block *malloc_block() {
  uint64 block_pa;
  if (current->block_page_size > sizeof(block)) {
    current->block_page_size -= sizeof(block);
    block_pa = current->next_block_pa;
    current->next_block_pa += sizeof(block);
  }
  else {
    current->next_block_pa = (uint64)alloc_page();
    current->block_page_size = PGSIZE;
    current->block_page_size -= sizeof(block);
    block_pa = current->next_block_pa;
    current->next_block_pa += sizeof(block);
  }
  return (block*)block_pa;
}

uint64 do_better_allocate(long n) {
  if (n > PGSIZE) panic("better_allocate failed,because your size is too large!\n");
  // block *pre = current->free_blocks, *cur = current->free_blocks;
  // while (cur != NULL) {
  //   if (cur->size >= n) break;
  //   pre = cur;
  //   cur = cur->next;
  // }
  block *pre, *cur;
  if (current->free_blocks == NULL) {
    //sprint("找不到空闲块\n");
    cur = malloc_block();
    cur->pa = (uint64)alloc_page();
    cur->va = g_ufree_page;
    g_ufree_page += PGSIZE;
    user_vm_map((pagetable_t)current->pagetable, cur->va, PGSIZE, (uint64)cur->pa, prot_to_type(PROT_WRITE | PROT_READ, 1));
    cur->size = PGSIZE;
    cur->next = current->free_blocks;
    current->free_blocks = cur;
    //sprint("空闲块分配完毕,大小为PGSIZE\n");
  }
  else {
    cur = current->free_blocks;
  }
  if (cur->size >= n) {
      block * temp = malloc_block();
      temp->pa = cur->pa + n;
      temp->va = cur->va + n;
      temp->size = cur->size - n;
      temp->next = cur->next;
      current->free_blocks = temp;
  }
  else {
      int left = n - cur->size;
      void* pa = alloc_page();
      uint64 va = g_ufree_page;
      g_ufree_page += PGSIZE;
      user_vm_map((pagetable_t)current->pagetable, cur->va+cur->size, left, (uint64)pa, prot_to_type(PROT_WRITE | PROT_READ, 1));
      block * temp = malloc_block();
      temp->pa = (uint64)pa + left;
      temp->va = va + left;
      temp->size = PGSIZE - left;
      temp->next = cur->next;
      current->free_blocks = temp;
  }
  cur->size -= n;
  cur->next = current->used_blocks;
  current->used_blocks = cur;
  //sprint("better allocate done\n");
  return cur->va;
}

void do_better_free(uint64 va) {
  //sprint("better free begin\n");
  block *pre, *cur = current->used_blocks;
  while (cur != NULL) {
    if (cur->va == va) break;
    pre = cur;
    cur = cur->next;
  }
  if (cur == NULL) panic("free failed,because the address you want to free is illegal!\n");
  cur->next = current->free_blocks;
  current->free_blocks = cur;
  //sprint("better free finish\n");
}