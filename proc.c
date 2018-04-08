#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

#define PRIORITY_MAX 1

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
  //Two Queues 0 and 1, associating each with priority level.
  struct proc * queue[2][NPROC];
  //two numbers, represnting the number of processes in each queue
  int priCount[2];
} ptable;


static struct proc *initproc;

int nextpid = 1;

int sched_trace_enabled = 1; // for CS550 CPU/process project
int sched_policy = 1; //default-value is told 1(MLFQ)
int RUNNING_THRESHOLD =2; // Default-value
int WAITING_THRESHOLD = 4; // Default-value
int init=0;

extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
  ptable.priCount[0] = -1;
  ptable.priCount[1] = -1;// No processes
}

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;
  char *sp;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;
  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;
  p->priority = 0;
  p->running_ticks = 0;
  p->waiting_ticks = 0;
  p->pinned = 1; //Non-Zero Value indicating normal scheduling policy.
  release(&ptable.lock);

  // Allocate kernel stack.
  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;
  
  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe*)sp;
  
  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context*)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;
  
  
  return p;
}

//PAGEBREAK: 32
// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];
  
  p = allocproc();
  initproc = p;
  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0;  // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");
  //Always new process is initialized to queue 0.
  /*--------------------*/
  p->queueNumber = 0;
  p->pinned = 1;
  ptable.priCount[0]++;
  ptable.queue[0][ptable.priCount[0]] = p;
  /*--------------------*/
  p->state = RUNNABLE;
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  
  sz = proc->sz;
  if(n > 0){
    if((sz = allocuvm(proc->pgdir, sz, sz + n)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(proc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  proc->sz = sz;
  switchuvm(proc);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void)
{
  int i, pid;
  struct proc *np;

  // Allocate process.
  if((np = allocproc()) == 0)
    return -1;

  // Copy process state from p.
  if((np->pgdir = copyuvm(proc->pgdir, proc->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  np->sz = proc->sz;
  np->parent = proc;
  *np->tf = *proc->tf;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(proc->ofile[i])
      np->ofile[i] = filedup(proc->ofile[i]);
  np->cwd = idup(proc->cwd);

  safestrcpy(np->name, proc->name, sizeof(proc->name));
 
  pid = np->pid;

  // lock to force the compiler to emit the np->state write last.
  acquire(&ptable.lock);
  np->queueNumber = 0;
  np->pinned = 1;
  ptable.priCount[0]++;
  ptable.queue[0][ptable.priCount[0]] = np;
  np->state = RUNNABLE;
  release(&ptable.lock);
  return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  struct proc *p;
  int fd;

  if(proc == initproc)
    panic("init exiting");

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(proc->ofile[fd]){
      fileclose(proc->ofile[fd]);
      proc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(proc->cwd);
  end_op();
  proc->cwd = 0;

  acquire(&ptable.lock);

  // Parent might be sleeping in wait().
  wakeup1(proc->parent);

  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == proc){
      p->parent = initproc;
      if(p->state == ZOMBIE){
        wakeup1(initproc);
      }
    }
  }
  //cprintf("Pid : [%d]\n",proc->pid);
  // Jump into the scheduler, never to return.
  proc->state = ZOMBIE;
  sched();
  
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  struct proc *p;
  int havekids, pid;

  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for zombie children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != proc)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->state = UNUSED;
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || proc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(proc, &ptable.lock);  //DOC: wait-sleep
  }
}
//Chooses between two policy
void choosePolicy(void){
  if(sched_policy==1){
    MLFQscheduler();
  }
  else if(sched_policy == 0){
    scheduler();
  }
}
//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  int ran = 0; // CS550: to solve the 100%-CPU-utilization-when-idling problem
  for(;;){
    // Enable interrupts on this processor.
    sti();

    // Loop over process table looking for process to run.
    acquire(&ptable.lock);
    ran = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->state != RUNNABLE)
        continue;

      ran = 1;
      
      // Switch to chosen process.  It is the process's job
      // to release ptable.lock and then reacquire it
      // before jumping back to us.
      proc = p;
      switchuvm(p);
      p->state = RUNNING;
      swtch(&cpu->scheduler, proc->context); // process will return from here when the process exits or when its quantum expires.
    //Quantum expires when it timer expires. it generates a trap( from trap.c)
      switchkvm();

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      proc = 0;
    

    }
    release(&ptable.lock);

    if (ran == 0){
        halt();
    }

  }
}

//MLFQScheduler
void MLFQscheduler(void){
    //cprintf("In MLFQ Scheduler\n");
    int ran= 0;
    struct proc *p;
    int qNumber;
    for(;;){
        // Enable interrupts on this processor.
        sti();
    
        // Loop over process table looking for process to run.
        acquire(&ptable.lock);
        



        ran =0;
        qNumber =0;
        for(qNumber = 0; qNumber <=1; qNumber++){

            while(ptable.priCount[qNumber]>-1){
            int index;
              for(index =0;index<=ptable.priCount[qNumber];index++){
                p = ptable.queue[qNumber][index];
                if(p->state!=RUNNABLE)
                  continue;
                else
                  break;
              }

              if(p->queueNumber == 0){
                int i;
                for (i = index; i < ptable.priCount[0]; i++) {
                    ptable.queue[0][i] = ptable.queue[0][i + 1];
                }
                ran=1;
                ptable.priCount[0]--;
                /*-------------------------- increasing waiting ticks for process in queue 1-------------*/
                struct proc *p0=0; 
                for(int k =0;k<=ptable.priCount[1];k++){
                    p0 = ptable.queue[1][k];
                    if(p0->state == RUNNABLE){
                      p0->waiting_ticks++;
                      p0->priority = p0->waiting_ticks;
                      ptable.queue[1][k] = p0;

                      if(p0->waiting_ticks == WAITING_THRESHOLD ){
                          ptable.priCount[p0->queueNumber]--;
                          p0->queueNumber =0;
                          p0->running_ticks =0;
                          p0->waiting_ticks =0;
                          p0->priority =0;
                          ptable.priCount[p0->queueNumber]++;
                          ptable.queue[0][ptable.priCount[0]] = p0;
                      }
                    }
                }
                /*----------------------------------------------------------------------------------------*/
                p->priority = 0;
        
                proc =p;
                
                switchuvm(p);
                p->state = RUNNING;
                //cprintf("Process : %d\n",proc->pid);
                //p->running_ticks++;
        
        
                //cprintf("PID: %d\t Running_ticks: %d\n",proc->pid,proc->running_ticks);
                //cprintf("HELLO\n");
                swtch(&cpu->scheduler, proc->context);
                
                switchkvm();


                
                proc = 0;
                qNumber =0;
              }
              else if(p->queueNumber == 1){

                //cprintf("I am Here\n");
                struct proc *p1=0;
                int ind=index;
                for(int f = 0;f<=ptable.priCount[1];f++){
                  p1 =ptable.queue[1][f];
                  if(p1!=p && p1->state == RUNNABLE){
                    //if(p1->priority >= p->priority){

                      /*if(p->priority == p1->priority){
                        ind= index;
                      }
                      else */if(p->priority < p1->priority){
                        p=p1;
                        ind= f;
                      }
                      
                    //}//second if


                  }//first if

                }//for loop

                for(int f =ind;f< ptable.priCount[1];f++){
                  ptable.queue[1][f] = ptable.queue[1][f+1];
                }
                ptable.priCount[p->queueNumber]--;

                //waiting ticks incrementing
                //struct proc *p2;
                /*for(int f=0; f<ptable.priCount[1];f++){
                    p2 = ptable.queue[1][f];
                    if(p!=p2){
                      p2->waiting_ticks++;
                      p2->priority = p2->waiting_ticks;
                      ptable.queue[1][f] = p2;
                      //cprintf("PID :[%d] and Waiting ticks [%d]\n",p2->pid,p2->waiting_ticks);
                    }
                }*/
                 

                ran=1;
                proc =p;
                switchuvm(p);
                p->state = RUNNING;
                
                //cprintf("PID: %d\t Running_ticks: %d\n",p->pid,p->running_ticks);
                swtch(&cpu->scheduler, proc->context);
                
                switchkvm();
                //we also need to increase the waiting tick of processes in Queue one that were not scheduled
                //printf("Scheduling done\n");
                proc = 0;
                p1=0;
                //p2=0;
                qNumber =0;
              }//else if end


            }
          
        }

    release(&ptable.lock);
    if (ran == 0){
      halt();
    }
    }
}
// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state.
void
sched(void)
{
  int intena;

  if(!holding(&ptable.lock)){
    
    panic("sched ptable.lock");
  }
  if(cpu->ncli != 1){
    
    panic("sched locks");
  }
  if(proc->state == RUNNING){
    
    panic("sched running");
  }
  if(readeflags()&FL_IF){
    
    panic("sched interruptible");
  }
  intena = cpu->intena;

  // CS550: print previously running process
  // We skip process 1 (init) and 2 (shell)
  //cprintf("PID: %d\t Running_ticks: %d\n",proc->pid,proc->running_ticks);
  
  //cprintf("Total Queue 0 Process : %d\n",ptable.priCount[0]);
  if(proc->pid != 1 && proc->pid != 2 && proc->queueNumber == 0 && proc->pinned != 0){
            proc->running_ticks++;
  }

  if(proc->pid != 1 && proc->pid != 2 && proc->queueNumber == 1 && proc->pinned != 0){
    struct proc *p2=0;
      for(p2 = ptable.proc; p2 < &ptable.proc[NPROC]; p2++){
        if(p2->state != RUNNABLE && p2->queueNumber != 1)
          continue;
        else{
          if(proc != p2){
            p2->waiting_ticks++;
            p2->priority = p2->waiting_ticks;
          }


                    //cprintf("PID :[%d] and Wait[%d] , ",p2->pid,p2->waiting_ticks);  
        }
      }
  }

  if ( sched_trace_enabled && proc && proc->pid != 1 && proc->pid != 2){
      cprintf("[%d]", proc->pid);
  }
  //cprintf("Hi\n");
  
  swtch(&proc->context, cpu->scheduler);
  cpu->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  //cprintf("Into yield.\n"); 
  acquire(&ptable.lock);  //DOC: yieldlock
  if(proc->waiting_ticks == WAITING_THRESHOLD && proc->queueNumber == 1 && proc->pid !=1 && proc->pid!=2){ //promotion to queue 0.
    //cprintf("Promoting process : %d\n",proc->pid);
    //proc->priority =0;
    proc->waiting_ticks = 0;
    proc->running_ticks =0;
    proc->queueNumber = 0;
    //ptable.priCount[0]++;
    //ptable.queue[0][ptable.priCount[0]]= proc;        
  }

  

  if(proc->running_ticks == RUNNING_THRESHOLD && proc->queueNumber==0 && proc->pid !=1 && proc->pid!=2){ //Demotion
    //cprintf("Demoting process : %d\n",proc->pid);
    proc->priority = 0;
    proc->waiting_ticks =0;
    proc->running_ticks = 0;
    proc->queueNumber = 1;
    //ptable.priCount[1]++;
    //ptable.queue[1][ptable.priCount[1]]= proc;
  }
  ptable.priCount[proc->queueNumber]++;
  ptable.queue[proc->queueNumber][ptable.priCount[proc->queueNumber]] = proc;
  proc->state = RUNNABLE;
  //cprintf("hey\n");
  sched();
  //cprintf("Sched done\n");
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first) {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot 
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }
  
  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  if(proc == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable.lock){  //DOC: sleeplock0
    acquire(&ptable.lock);  //DOC: sleeplock1
    release(lk);
  }

  // Go to sleep.
  proc->chan = chan;
  proc->state = SLEEPING;
  sched();

  // Tidy up.
  proc->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == SLEEPING && p->chan == chan){
    ptable.priCount[p->queueNumber]++;
    ptable.queue[p->queueNumber][ptable.priCount[p->queueNumber]] = p;
  p->state = RUNNABLE;
  }
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid)
{
  struct proc *p;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->killed = 1;
      // Wake process from sleep if necessary.
      if(p->state == SLEEPING ){
        ptable.priCount[p->queueNumber]++;
        ptable.queue[p->queueNumber][ptable.priCount[p->queueNumber]] = p;
    p->state = RUNNABLE;
    }
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [EMBRYO]    "embryo",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  int i;
  struct proc *p;
  char *state;
  uint pc[10];
  
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s %d", p->pid, state, p->name, p->priority);
    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}

int setrunningticks(int t){
  RUNNING_THRESHOLD = t;
  //cprintf("RUNNING_THRESHOLD : %d\n",RUNNING_THRESHOLD);
  
  return 0;
}

int setwaitingticks(int t){
  WAITING_THRESHOLD = t;
    //cprintf("WAITING_THRESHOLD : %d\n",WAITING_THRESHOLD);
  return 0;
}

int setpriority(int p_id, int pri){
  
  acquire(&ptable.lock);
      if(proc->pid == p_id){
        //cprintf("I found it\n");
        if(proc->pinned !=0 && pri ==0){
          if(proc->queueNumber ==0){
            //cprintf("10\n");
            proc->pinned = pri;
            proc->running_ticks =0;
            proc->waiting_ticks = 0;
            //ptable.queue[proc->queueNumber][ptable.priCount[proc->queueNumber]] = proc;

          }
          else if (proc->queueNumber == 1){
            //ptable.priCount[proc->queueNumber]--;
            proc->pinned = pri;
            proc->running_ticks =0;
            proc->waiting_ticks = 0;
            proc->queueNumber =0;
            //cprintf("11\n");

          }

        }
        else if(proc->pinned ==0 && pri !=0){
          proc->pinned =pri;
          //cprintf("12\n");
          //ptable.queue[proc->queueNumber][ptable.priCount[proc->queueNumber]] = proc;
        }
        //ptable.queue[proc->queueNumber][ptable.priCount[proc->queueNumber]] = proc;
      }

  release(&ptable.lock);

  return 0;
}