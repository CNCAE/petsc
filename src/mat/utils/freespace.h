#if !defined(_FreeSpace_h_)
#define _FreeSpace_h_

#include "petsc.h"

typedef struct _Space *PetscFreeSpaceList;

struct _Space {
  PetscFreeSpaceList more_space;
  PetscInt           *array;
  PetscInt           *array_head;
  PetscInt           total_array_size;
  PetscInt           local_used;
  PetscInt           local_remaining;
};

PetscErrorCode PetscFreeSpaceGet(PetscInt,PetscFreeSpaceList*);
PetscErrorCode PetscFreeSpaceContiguous(PetscFreeSpaceList*,PetscInt *);
PetscErrorCode PetscFreeSpaceContiguous_newdatastruct(PetscFreeSpaceList*,PetscInt*,PetscInt,PetscInt*,PetscInt*);
PetscErrorCode PetscFreeSpaceDestroy(PetscFreeSpaceList);

#endif
