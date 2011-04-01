#include "taosolver.h"
typedef struct {
  PetscInt n;
  PetscInt m;
  PetscInt mx; // grid points in each direction
  IS s_is;
  IS d_is;
  VecScatter state_scatter;
  VecScatter design_scatter;

  PetscReal alpha;
  Mat Q;
  Mat L;
  Mat Div;
  Mat Grad;
  Mat Av;
  Vec q;
  Vec ur; // reference

  Vec d;
  Vec dwork;

  Vec y; // state variables
  Vec ywork;

  Vec u; // design variables
  Vec uwork;
  
  Vec c; // constraint vector
  Vec cwork;
  
  Vec lwork;
  Vec S;
  Vec Swork;

} AppCtx;

PetscErrorCode FormFunction(TaoSolver, Vec, PetscReal*, void*);
PetscErrorCode FormGradient(TaoSolver, Vec, Vec, void*);
PetscErrorCode FormFunctionGradient(TaoSolver, Vec, PetscReal*, Vec, void*);
PetscErrorCode FormJacobian(TaoSolver, Vec, Mat*, Mat*, MatStructure*,void*);
PetscErrorCode FormConstraints(TaoSolver, Vec, Vec, void*);
PetscErrorCode FormHessian(TaoSolver, Vec, Mat*, Mat*, MatStructure*, void*);
PetscErrorCode Gather(Vec x, Vec state, VecScatter s_scat, Vec design, VecScatter d_scat);
PetscErrorCode Scatter(Vec x, Vec state, VecScatter s_scat, Vec design, VecScatter d_scat);
PetscErrorCode ESQPInitialize(AppCtx *user);
PetscErrorCode ESQPDestroy(AppCtx *user);

PetscErrorCode ESQPMatMult(Mat,Vec,Vec);

static  char help[]="";

#undef __FUNCT__
#define __FUNCT__ "main"
int main(int argc, char **argv)
{
  PetscErrorCode ierr;
  Vec x;
  Mat J;
  PetscInt lo,hi;
  TaoSolver tao;
  TaoSolverTerminationReason reason;
  AppCtx user;
  IS is_allstate,is_alldesign;
  PetscInt idx[] = {0,1};

  PetscInitialize(&argc, &argv, (char*)0,help);
  TaoInitialize(&argc, &argv, (char*)0,help);

  user.mx = 8;
  user.m = user.mx*user.mx*user.mx; // number of constraints
  user.n = 2*user.m; // number of variables

  ierr = ISCreateGeneral(PETSC_COMM_SELF,user.m,idx,PETSC_COPY_VALUES,&user.s_is); CHKERRQ(ierr);
  
  ierr = VecCreateSeq(PETSC_COMM_SELF,user.n,&x); CHKERRQ(ierr);
  ierr = VecCreateSeq(PETSC_COMM_SELF,user.m,&c); CHKERRQ(ierr);
  ierr = VecGetOwnershipRange(x,&lo,&hi); CHKERRQ(ierr);
  ierr = ISComplement(user.s_is,lo,hi,&user.d_is); CHKERRQ(ierr);

  /* Create scatters for reduced spaces */
  ierr = VecGetOwnershipRange(x,&lo,&hi); CHKERRQ(ierr);
  ierr = ISCreateStride(PETSC_COMM_SELF,hi-lo,lo,1,&is_allstate); CHKERRQ(ierr);
  ierr = VecGetOwnershipRange(user.u,&lo,&hi); CHKERRQ(ierr);
  ierr = ISCreateStride(PETSC_COMM_SELF,hi-lo,lo,1,&is_alldesign); CHKERRQ(ierr);
  ierr = VecCreateSeq(PETSC_COMM_SELF,user.n,&user.u); CHKERRQ(ierr);
  ierr = VecCreateSeq(PETSC_COMM_SELF,user.m,&user.y); CHKERRQ(ierr);
  ierr = VecCreateSeq(PETSC_COMM_SELF,user.m,&user.c); CHKERRQ(ierr);

  ierr = VecScatterCreate(x,user.s_is,user.y,is_allstate,&user.state_scatter); CHKERRQ(ierr);
  ierr = VecScatterCreate(x,user.d_is,user.u,is_alldesign,&user.design_scatter); CHKERRQ(ierr);
  ierr = ISDestroy(is_alldesign); CHKERRQ(ierr);
  ierr = ISDestroy(is_allstate); CHKERRQ(ierr);

  /* Set up initial vectors and matrices */
  ierr = ESQPInitialize(&user); CHKERRQ(ierr);
  ierr = Gather(x,user.y,user.state_scatter,user.u,user.design_scatter); CHKERRQ(ierr);


  ierr = MatCreateSeqAIJ(PETSC_COMM_SELF, user.m, user.n, user.n, PETSC_NULL,&J); CHKERRQ(ierr);
  ierr = MatAssemblyBegin(J,MAT_FINAL_ASSEMBLY); CHKERRQ(ierr);
  ierr = MatAssemblyEnd(J,MAT_FINAL_ASSEMBLY); CHKERRQ(ierr);

  /* Create TAO solver and set desired solution method */
  ierr = TaoSolverCreate(PETSC_COMM_SELF,&tao); CHKERRQ(ierr);
  ierr = TaoSolverSetType(tao,"tao_rsqn"); CHKERRQ(ierr);

  /* Set solution vector with an initial guess */
  ierr = TaoSolverSetInitialVector(tao,x); CHKERRQ(ierr);
  ierr = TaoSolverSetObjectiveRoutine(tao, FormFunction, (void *)&user); CHKERRQ(ierr);
  ierr = TaoSolverSetGradientRoutine(tao, FormGradient, (void *)&user); CHKERRQ(ierr);
  ierr = TaoSolverSetConstraintsRoutine(tao, c, FormConstraints, (void *)&user); CHKERRQ(ierr);

  ierr = TaoSolverSetJacobianStateRoutine(tao, J, J, FormJacobian, (void *)&user); CHKERRQ(ierr);
  ierr = TaoSolverSetJacobianDesignRoutine(tao, J, J, FormJacobian, (void *)&user); CHKERRQ(ierr);

  ierr = TaoSolverRSQNSetStateIS(tao,user.s_is); CHKERRQ(ierr);
  ierr = TaoSolverSetFromOptions(tao); CHKERRQ(ierr);

  /* SOLVE THE APPLICATION */
  ierr = TaoSolverSolve(tao);  CHKERRQ(ierr);

  ierr = TaoSolverGetConvergedReason(tao,&reason); CHKERRQ(ierr);

  if (reason < 0)
  {
    PetscPrintf(MPI_COMM_SELF, "Try a different TAO method. RSQN failed.\n");
  }
  else
  {
    PetscPrintf(MPI_COMM_SELF, "Optimization terminated with status %2d.\n", reason);
  }


  /* Free TAO data structures */
  ierr = TaoSolverDestroy(tao); CHKERRQ(ierr);

  /* Free PETSc data structures */
  ierr = VecDestroy(x); CHKERRQ(ierr);
  ierr = VecDestroy(c); CHKERRQ(ierr);
  ierr = ESQPDestroy(&user); CHKERRQ(ierr);
  ierr = MatDestroy(J); CHKERRQ(ierr);

  /* Finalize TAO, PETSc */
  TaoFinalize();
  PetscFinalize();

  return 0;
}
/* ------------------------------------------------------------------- */
#undef __FUNCT__
#define __FUNCT__ "FormFunction"
/* 
   dwork = Qy - d  
   lwork = L*(u-ur)
   f = 1/2 * (dwork.dork + alpha*lwork.lwork)
*/
PetscErrorCode FormFunction(TaoSolver tao,Vec X,PetscReal *f,void *ptr)
{
  PetscErrorCode ierr;
  PetscScalar d1,d2;
  AppCtx *user = (AppCtx*)ptr;
  PetscFunctionBegin;
  ierr = Scatter(X,user->y,user->state_scatter,user->u,user->design_scatter); CHKERRQ(ierr);
  ierr = MatMult(user->Q,user->y,user->dwork); CHKERRQ(ierr);
  ierr = VecAXPY(user->dwork,-1.0,user->d); CHKERRQ(ierr);
  ierr = VecDot(user->dwork,user->dwork,&d1); CHKERRQ(ierr);

  ierr = VecWAXPY(user->uwork,-1.0,user->ur,user->u); CHKERRQ(ierr);
  ierr = MatMult(user->L,user->uwork,user->lwork); CHKERRQ(ierr);
  ierr = VecDot(user->lwork,user->lwork,&d1); CHKERRQ(ierr);
  
  *f = 0.5 * (d1 + user->alpha*d2); 
  
  PetscFunctionReturn(0);
}
p
/* ------------------------------------------------------------------- */
#undef __FUNCT__
#define __FUNCT__ "FormGradient"
/*  
    state: g_s = Q' *(Qy - d)
    design: g_d = alpha*L'*L*(u-ur)
*/
PetscErrorCode FormGradient(TaoSolver tao,Vec X,Vec G,void *ptr)
{
  PetscErrorCode ierr;
  AppCtx *user = (AppCtx*)ptr;
  PetscFunctionBegin;
  ierr = Scatter(X,user->y,user->state_scatter,user->u,user->design_scatter); CHKERRQ(ierr);
  ierr = MatMult(user->Q,user->y,user->dwork); CHKERRQ(ierr);
  ierr = VecAXPY(user->dwork,-1.0,user->d); CHKERRQ(ierr);
  ierr = MatMultTranspose(user->Q,user->dwork,user->ywork); CHKERRQ(ierr);
  
  ierr = VecWAXPY(user->uwork,-1.0,user->ur,user->u); CHKERRQ(ierr);
  ierr = MatMult(user->L,user->uwork,user->lwork); CHKERRQ(ierr);
  ierr = MatMultTranspose(user->L,user->lwork,user->uwork); CHKERRQ(ierr);
  ierr = VecScale(user->uwork, user->alpha); CHKERRQ(ierr);

		      
  ierr = Gather(G,user->ywork,user->state_scatter,user->uwork,user->design_scatter); CHKERRQ(ierr);
  PetscFunctionReturn(0);
}

#undef __FUNCT__
#define __FUNCT__ "FormFunctionGradient"
PetscErrorCode FormFunctionGradient(TaoSolver tao, Vec X, PetscScalar *f, Vec G, void *ptr)
{
  PetscErrorCode ierr;
  PetscScalar d1,d2;
  AppCtx *user = (AppCtx*)ptr;
  PetscFunctionBegin;
  ierr = Scatter(X,user->y,user->state_scatter,user->u,user->design_scatter); CHKERRQ(ierr);

  ierr = MatMult(user->Q,user->y,user->dwork); CHKERRQ(ierr);
  ierr = VecAXPY(user->dwork,-1.0,user->d); CHKERRQ(ierr);
  ierr = VecDot(user->dwork,user->dwork,&d1); CHKERRQ(ierr);
  ierr = MatMultTranspose(user->Q,user->dwork,user->ywork); CHKERRQ(ierr);

  ierr = VecWAXPY(user->uwork,-1.0,user->ur,user->u); CHKERRQ(ierr);
  ierr = MatMult(user->L,user->uwork,user->lwork); CHKERRQ(ierr);
  ierr = VecDot(user->lwork,user->lwork,&d1); CHKERRQ(ierr);
  ierr = MatMultTranspose(user->L,user->lwork,user->uwork); CHKERRQ(ierr);
  ierr = VecScale(user->uwork, user->alpha); CHKERRQ(ierr);
  *f = 0.5 * (d1 + user->alpha*d2); 



  ierr = Gather(G,user->ywork,user->state_scatter,user->uwork,user->design_scatter); CHKERRQ(ierr);
  PetscFunctionReturn(0);

}
/* ------------------------------------------------------------------- */
#undef __FUNCT__
#define __FUNCT__ "FormJacobian"
PetscErrorCode FormJacobian(TaoSolver tao, Vec X, Mat *J, Mat* JPre, MatStructure* flag, void *ptr)
{
  PetscErrorCode ierr;
  AppCtx *user = (AppCtx*)ptr;
  PetscFunctionBegin;
  ierr = Scatter(X,user->y,user->state_scatter,user->u,user->design_scatter); CHKERRQ(ierr);

  PetscFunctionReturn(0);
}


#undef __FUNCT__
#define __FUNCT__ "ESQPMatMult"
PetscErrorCode ESQPMatMult(Mat J_shell, Vec X, Vec Y) 
{
  PetscErrorCode ierr;
  void *ptr;
  AppCtx *user;
  PetscFunctionBegin;
  ierr = MatShellGetContext(J_shell,&ptr); CHKERRQ(ierr);
  user = (AppCtx*)ptr;
  ierr = Scatter(X,user->y,user->state_scatter,user->u,user->design_scatter); CHKERRQ(ierr);
  ierr = VecScale(user->u,-1.0); CHKERRQ(ierr);
  ierr = VecExp(user->u); CHKERRQ(ierr);
  ierr = MatMult(user->Av,user->u,user->S); CHKERRQ(ierr):

  ierr = MatMult(user->Grad,user->y,user->Swork); CHKERRQ(ierr);
  ierr = VecPointwiseDivide(user->S,user->Swork,user->S); CHKERRQ(ierr);
  ierr = MatMult(user->Div,user->S,Y); CHKERRQ(ierr);
  ierr = VecSum(user->y,&sum); CHKERRQ(ierr);
  sum /= user->n*user->n*user->n;
  ierr = VecShift(Y,sum); CHKERRQ(ierr);
  PetscFunctionReturn(0);
}

#undef __FUNCT__
#define __FUNCT__ "FormConstraints"
PetscErrorCode FormConstraints(TaoSolver tao, Vec X, Vec C, void *ptr)
{
  // C=Ay - q      A = Div * Sigma * Av - hx*hx*hx*ones(n,n)
   PetscErrorCode ierr;
   PetscScalar sum;
   AppCtx *user = (AppCtx*)ptr;
   PetscFunctionBegin;

   ierr = MatMult(user->J,X,C); CHKERRQ(ierr);
   /*
   ierr = Scatter(X,user->y,user->state_scatter,user->u,user->design_scatter); CHKERRQ(ierr);
   ierr = VecScale(user->u,-1.0); CHKERRQ(ierr);
   ierr = VecExp(user->u); CHKERRQ(ierr);
   ierr = MatMult(user->Av,user->u,user->S); CHKERRQ(ierr):

   ierr = MatMult(user->Grad,user->y,user->Swork); CHKERRQ(ierr);
   ierr = VecPointwiseDivide(user->S,user->Swork,user->S); CHKERRQ(ierr);
   ierr = MatMult(user->Div,user->S,C); CHKERRQ(ierr);
   ierr = VecSum(user->y,&sum); CHKERRQ(ierr);
   sum /= user->n*user->n*user->n;
   ierr = VecShift(C,sum); CHKERRQ(ierr);
   */

   ierr = VecAxpy(C,-1.0,user->q); CHKERRQ(ierr);

   PetscFunctionReturn(0);
}

#undef __FUNCT__
#define __FUNCT__ "Scatter"
PetscErrorCode Scatter(Vec x, Vec state, VecScatter s_scat, Vec design, VecScatter d_scat)
{
  PetscErrorCode ierr;
  PetscFunctionBegin;
  ierr = VecScatterBegin(s_scat,x,state,INSERT_VALUES,SCATTER_FORWARD); CHKERRQ(ierr);
  ierr = VecScatterEnd(s_scat,x,state,INSERT_VALUES,SCATTER_FORWARD); CHKERRQ(ierr);

  ierr = VecScatterBegin(d_scat,x,design,INSERT_VALUES,SCATTER_FORWARD); CHKERRQ(ierr);
  ierr = VecScatterEnd(d_scat,x,design,INSERT_VALUES,SCATTER_FORWARD); CHKERRQ(ierr);
  PetscFunctionReturn(0);
}


#undef __FUNCT__
#define __FUNCT__ "Gather"
PetscErrorCode Gather(Vec x, Vec state, VecScatter s_scat, Vec design, VecScatter d_scat)
{
  PetscErrorCode ierr;
  PetscFunctionBegin;
  ierr = VecScatterBegin(s_scat,state,x,INSERT_VALUES,SCATTER_REVERSE); CHKERRQ(ierr);
  ierr = VecScatterEnd(s_scat,state,x,INSERT_VALUES,SCATTER_REVERSE); CHKERRQ(ierr);
  ierr = VecScatterBegin(d_scat,design,x,INSERT_VALUES,SCATTER_REVERSE); CHKERRQ(ierr);
  ierr = VecScatterEnd(d_scat,design,x,INSERT_VALUES,SCATTER_REVERSE); CHKERRQ(ierr);
  PetscFunctionReturn(0);
}
  
    
#undef __FUNCT__
#define __FUNCT__ "ESQPInitialize"
PetscErrorCode ESQPInitialize(AppCtx *user)
{
  PetscErrorCode ierr;
  PetscViewer reader;
  PetscFunctionBegin;
  user->alpha = 0.l;
  ierr = PetscViewerBinaryOpen(PETSC_COMM_SELF,"u0.dat",FILE_MODE_READ,&reader); CHKERRQ(ierr);
  ierr = VecCreate(PETSC_COMM_WORLD,&user->u); CHKERRQ(ierr);
  ierr = VecLoad(user->u,reader); CHKERRQ(ierr);
  ierr = PetscViewerDestroy(reader); CHKERRQ(ierr);

  ierr = PetscViewerBinaryOpen(PETSC_COMM_SELF,"ur.dat",FILE_MODE_READ,&reader); CHKERRQ(ierr);
  ierr = VecCreate(PETSC_COMM_WORLD,&user->ur); CHKERRQ(ierr);
  ierr = VecLoad(user->ur,reader); CHKERRQ(ierr);
  ierr = PetscViewerDestroy(reader); CHKERRQ(ierr);
  
  ierr = PetscViewerBinaryOpen(PETSC_COMM_SELF,"y0.dat",FILE_MODE_READ,&reader); CHKERRQ(ierr);
  ierr = VecCreate(PETSC_COMM_WORLD,&user->y); CHKERRQ(ierr);
  ierr = VecLoad(user->y,reader); CHKERRQ(ierr);
  ierr = PetscViewerDestroy(reader); CHKERRQ(ierr);
  
  ierr = PetscViewerBinaryOpen(PETSC_COMM_SELF,"q.dat",FILE_MODE_READ,&reader); CHKERRQ(ierr);
  ierr = VecCreate(PETSC_COMM_WORLD,&user->q); CHKERRQ(ierr);
  ierr = VecLoad(user->q,reader); CHKERRQ(ierr);
  ierr = PetscViewerDestroy(reader); CHKERRQ(ierr);

  ierr = PetscViewerBinaryOpen(PETSC_COMM_SELF,"d.dat",FILE_MODE_READ,&reader); CHKERRQ(ierr);
  ierr = VecCreate(PETSC_COMM_WORLD,&user->d); CHKERRQ(ierr);
  ierr = VecLoad(user->d,reader); CHKERRQ(ierr);
  ierr = PetscViewerDestroy(reader); CHKERRQ(ierr);
  
  ierr = PetscViewerBinaryOpen(PETSC_COMM_SELF,"Q.dat",FILE_MODE_READ,&reader); CHKERRQ(ierr);
  ierr = MatCreate(PETSC_COMM_WORLD,&user->Q); CHKERRQ(ierr);
  ierr = MatLoad(user->Q, reader); CHKERRQ(ierr);
  ierr = PetscViewerDestroy(reader); CHKERRQ(ierr);


  ierr = PetscViewerBinaryOpen(PETSC_COMM_SELF,"L.dat",FILE_MODE_READ,&reader); CHKERRQ(ierr);
  ierr = MatCreate(PETSC_COMM_WORLD,&user->L); CHKERRQ(ierr);
  ierr = MatLoad(user->L, reader); CHKERRQ(ierr);
  ierr = PetscViewerDestroy(reader); CHKERRQ(ierr);

  ierr = VecCreateSeq(PETSC_COMM_SELF,1856,&user->lwork); CHKERRQ(ierr);
  ierr = VecCreateSeq(PETSC_COMM_SELF,user.n*user.n*(user.n-1)*3, &user->S); CHKERRQ(ierr);

  ierr = PetscViewerBinaryOpen(PETSC_COMM_SELF,"Grad.dat",FILE_MODE_READ,&reader); CHKERRQ(ierr);
  ierr = MatCreate(PETSC_COMM_WORLD,&user->Grad); CHKERRQ(ierr);
  ierr = MatLoad(user->Grad, reader); CHKERRQ(ierr);
  ierr = PetscViewerDestroy(reader); CHKERRQ(ierr);

  ierr = PetscViewerBinaryOpen(PETSC_COMM_SELF,"Div.dat",FILE_MODE_READ,&reader); CHKERRQ(ierr);
  ierr = MatCreate(PETSC_COMM_WORLD,&user->Div); CHKERRQ(ierr);
  ierr = MatLoad(user->Div, reader); CHKERRQ(ierr);
  ierr = PetscViewerDestroy(reader); CHKERRQ(ierr);

  ierr = PetscViewerBinaryOpen(PETSC_COMM_SELF,"Av.dat",FILE_MODE_READ,&reader); CHKERRQ(ierr);
  ierr = MatCreate(PETSC_COMM_WORLD,&user->Av); CHKERRQ(ierr);
  ierr = MatLoad(user->Av, reader); CHKERRQ(ierr);
  ierr = PetscViewerDestroy(reader); CHKERRQ(ierr);

  ierr = VecDuplicate(user->S,&user->Swork); CHKERRQ(ierr);
  ierr = VecDuplicate(user->y,&user->ywork); CHKERRQ(ierr);
  ierr = VecDuplicate(user->u,&user->uwork); CHKERRQ(ierr);
  ierr = VecDuplicate(user->c,&user->cwork); CHKERRQ(ierr);

  ierr = MatCreateShell(PETSC_COMM_SELF,PETSC_DETERMINE,PETSC_DETERMINE,user->m,2*user->n,&user,&user->J); CHKERRQ(ierr);
  ierr = MatShellSetOperation(user->J,MATOP_MULT,ESQPMatMult); CHKERRQ(ierr);
  PetscFunctionReturn(0);
}

#undef __FUNCT__
#define __FUNCT__ "ESQPDestroy"
PetscErrorCode ESQPDestroy(AppCtx *user)
{
  PetscErrorCode ierr;
  PetscFunctionBegin;
  ierr = MatDestroy(user->Q); CHKERRQ(ierr);
  ierr = MatDestroy(user->Div); CHKERRQ(ierr);
  ierr = MatDestroy(user->Grad); CHKERRQ(ierr);
  ierr = MatDestroy(user->Av); CHKERRQ(ierr);
  ierr = MatDestroy(user->L); CHKERRQ(ierr);
  ierr = VecDestroy(user->u); CHKERRQ(ierr);
  ierr = VecDestroy(user->uwork); CHKERRQ(ierr);
  ierr = VecDestroy(user->y); CHKERRQ(ierr);
  ierr = VecDestroy(user->ywork); CHKERRQ(ierr);
  ierr = VecDestroy(user->c); CHKERRQ(ierr);
  ierr = VecDestroy(user->cwork); CHKERRQ(ierr);
  ierr = VecDestroy(user->ur); CHKERRQ(ierr);
  ierr = VecDestroy(user->q); CHKERRQ(ierr);
  ierr = VecDestroy(user->d); CHKERRQ(ierr);
  ierr = VecDestroy(user->dwork); CHKERRQ(ierr);
  ierr = VecDestroy(user->lwork); CHKERRQ(ierr);
  ierr = VecDestroy(user->S); CHKERRQ(ierr);
  ierr = VecDestroy(user->Swork); CHKERRQ(ierr);
  ierr = ISDestroy(user->s_is); CHKERRQ(ierr);
  ierr = ISDestroy(user->d_is); CHKERRQ(ierr);
  ierr = VecScatterDestroy(user->state_scatter); CHKERRQ(ierr);
  ierr = VecScatterDestroy(user->design_scatter); CHKERRQ(ierr);
  PetscFunctionReturn(0);
}
