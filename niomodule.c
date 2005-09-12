/*
 * Objects representing NIO files and variables.
 *
 * David I. Brown
 * Adapted from netcdfmodule.c which was
 * Written by Konrad Hinsen
 * last revision: 1998-3-14
 */


#include "netcdf.h"
#include "Python.h"
#include "nio.h"
#include <Numeric/arrayobject.h>
#include <sys/stat.h>
#include <unistd.h>


#define _NIO_MODULE
#include "niomodule.h"

/*
 * global used in NclMultiDValData
 */
short NCLnoPrintElem = 0;

staticforward int nio_file_init(PyNIOFileObject *self);
staticforward PyNIOVariableObject *nio_variable_new(
	PyNIOFileObject *file, char *name, int id, 
	int type, int ndims, int *dimids, int nattrs);


/* Error object and error messages for nio-specific errors */

static PyObject *NIOError;

#if 0
static char *nio_errors[] = {
  "No Error",
  "Not a nio id",
  "Too many nios open",
  "nio file exists && NC_NOCLOBBER",
  "Invalid Argument",
  "Write to read only",
  "Operation not allowed in data mode",
  "Operation not allowed in define mode",
  "Coordinates out of Domain",
  "MAX_NC_DIMS exceeded",
  "String match to name in use",
  "Attribute not found",
  "MAX_NC_ATTRS exceeded",
  "Not a nio data type",
  "Invalid dimension id",
  "NC_UNLIMITED in the wrong index",
  "MAX_NC_VARS exceeded",
  "Variable not found",
  "Action prohibited on NC_GLOBAL varid",
  "Not a nio file",
  "In Fortran, string too short",
  "MAX_NC_NAME exceeded",
  "NC_UNLIMITED size already in use", /* 22 */
  "", "", "", "", "", "", "", "", "",
  "XDR error" /* 32 */
};
#endif

/* Set error string */
static void
nio_seterror(void)
{
  int ncerr = 1;
  if (ncerr != 0) {
    char *error = "Unknown error";
    #if 0
    if (ncerr > 0 && ncerr <= 32)
      error = nio_errors[ncerr];
    #endif
    PyErr_SetString(NIOError, error);
  }
}

/*
 * Python equivalents to NIO data types
 *
 * Attention: the following specification may not be fully portable.
 * The comments indicate the correct NIO specification. The assignment
 * of Python types assumes that 'short' is 16-bit and 'int' is 32-bit.
 */

#if 0
int data_types[] = {-1,  /* not used */
		    PyArray_SBYTE,  /* signed 8-bit int */
		    PyArray_CHAR,   /* 8-bit character */
		    PyArray_SHORT,  /* 16-bit signed int */
		    PyArray_INT,    /* 32-bit signed int */
		    PyArray_FLOAT,  /* 32-bit IEEE float */
		    PyArray_DOUBLE  /* 64-bit IEEE float */
};
#endif

int data_type(NclBasicDataTypes ntype)
{
	switch (ntype) {
	case NCL_short:
		return PyArray_SHORT;
	case NCL_int:
		return PyArray_INT;  /* netcdf 3.x has only a long type */
	case NCL_long:
		return PyArray_LONG;
	case NCL_float:
		return PyArray_FLOAT;
	case NCL_double:
		return PyArray_DOUBLE;
        case NCL_char:
		return PyArray_CHAR;
        case NCL_byte:
		return PyArray_UBYTE;
        case NCL_string:
		return PyArray_CHAR;
	default:
		return PyArray_NOTYPE;
	}
	return PyArray_NOTYPE;
}



/* Utility functions */

static void
define_mode(PyNIOFileObject *file, int define_flag)
{
  if (file->define != define_flag) {
#if 0
    if (file->define)
      ncendef(file->id);
    else
      ncredef(file->id);
#endif
    file->define = define_flag;
  }
}


static char
typecode(int type)
{
  char t;
  switch(type) {
  case PyArray_CHAR:
    t = 'c';
    break;
  case PyArray_UBYTE:
    t = 'b';
    break;
  case PyArray_SBYTE:
    t = '1';
    break;
  case PyArray_SHORT:
    t = 's';
    break;
  case PyArray_INT:
    t = 'i';
    break;
  case PyArray_LONG:
    t = 'l';
    break;
  case PyArray_FLOAT:
    t = 'f';
    break;
  case PyArray_DOUBLE:
    t = 'd';
    break;
  default: t = ' ';
  }
  return t;
}

static NrmQuark
nio_type_from_code(char code)
{
  int type;
  switch(code) {
  case 'c':
	  type = NrmStringToQuark("character");
	  break;
  case 'b':
  case '1':
	  type = NrmStringToQuark("byte");
	  break;
  case 's':
	  type = NrmStringToQuark("short");
	  break;
  case 'i':
	  type = NrmStringToQuark("integer");
	  break;
  case 'l':
	  type = NrmStringToQuark("long");
	  break;
  case 'f':
	  type = NrmStringToQuark("float");
	  break;
  case 'd':
	  type = NrmStringToQuark("double");
	  break;
  default:
	  type = NrmNULLQUARK;
  }
  return type;
}


static void
collect_attributes(int fileid, int varid, PyObject *attributes, int nattrs)
{
  NclFile file = (NclFile) fileid;
  NclFileAttInfoList *att_list = NULL;
  NclFAttRec *att;
  NclFVarRec *fvar = NULL;
  char *name;
  int length;
  int py_type;
  int i;
  if (varid > -1) {
	  att_list = file->file.var_att_info[varid];
	  fvar = file->file.var_info[varid];
  }
  for (i = 0; i < nattrs; i++) {
	  NclMultiDValData md;
	  if (varid < 0) {
		  att = file->file.file_atts[i];
		  name = NrmQuarkToString(att->att_name_quark);
		  md = _NclFileReadAtt(file,att->att_name_quark,NULL);
	  }
	  else if (! (att_list && fvar)) {
		  PyErr_SetString(NIOError, "internal attribute or file variable error");
		  return;
	  }
	  else {
		  att = att_list->the_att;
		  name = NrmQuarkToString(att->att_name_quark);
		  md = _NclFileReadVarAtt(file,fvar->var_name_quark,att->att_name_quark,NULL);
		  att_list = att_list->next;
	  }
	  if (att->data_type == NCL_string) {
		  char *satt = NrmQuarkToString(*((NrmQuark *)md->multidval.val));
		  char *s = (char *)malloc((strlen(satt)+1)*sizeof(char));
		  if (s != NULL) {
			  PyObject *string;
			  strcpy(s,satt);
			  string = PyString_FromString(s);
			  if (string != NULL) {
				  PyDict_SetItemString(attributes, name, string);
				  Py_DECREF(string);
			  }
		  }
	  }
	  else {
		  length = md->multidval.totalelements;
		  py_type = data_type(att->data_type);
		  PyObject *array = PyArray_FromDims(1, &length, py_type);
		  if (array != NULL) {
			  memcpy(((PyArrayObject *)array)->data,
				 md->multidval.val,length * md->multidval.type->type_class.size);
			  array = PyArray_Return((PyArrayObject *)array);
			  if (array != NULL) {
				  PyDict_SetItemString(attributes, name, array);
				  Py_DECREF(array);
			  }
		  }
	  }
  }
}
#if 0
static NclMultiDValData 
PyValueToMultiDVal(PyObject *val)
{
  NclMultiDValData md;

  if (PyString_Check(val)) {
	  int len_dims = 1;
	  NrmQuark *qval = malloc(sizeof(NrmQuark));
	  qval[0] = NrmStringToQuark(PyString_AsString(val));
	  md = _NclCreateMultiDVal(NULL,NULL,Ncl_MultiDValData,0,
				   (void*)qval,NULL,1,&len_dims,
				   TEMPORARY,NULL,(NclTypeClass)nclTypestringClass);
  }
  else {
	  int n_dims;
	  int dim_sizes = 1;
	  NrmQuark qtype;
	  PyArrayObject *array =
		  (PyArrayObject *)PyArray_ContiguousFromObject(val, PyArray_NOTYPE, 0, 1);
	  n_dims = (array->nd == 0) ? 1 : array->nd;
	  qtype = nio_type_from_code(array->descr->type);
	  md = _NclCreateMultiDVal(NULL,NULL,Ncl_MultiDValData,0,
				   (void*)array->data,NULL,n_dims,
				   array->nd == 0 ? &dim_sizes : array->dimensions,
				   TEMPORARY,NULL,_NclNameToTypeClass(qtype));
  }
  return md;
	  
}
#endif

static int
set_attribute(PyNIOFileObject *file, int varid, PyObject *attributes,
	      char *name, PyObject *value)
{
  NclFile nfile = (NclFile) file->id;
  NhlErrorTypes ret;
  NclMultiDValData md;
  PyArrayObject *array = NULL;
  
  if (!value) {
	  /* delete attribute */
	  if (varid == NC_GLOBAL) {
		  ret = _NclFileDeleteAtt(nfile,NrmStringToQuark(name));
	  }
	  else {
		  ret = _NclFileDeleteVarAtt(nfile,nfile->file.var_info[varid]->var_name_quark,
					    NrmStringToQuark(name));
	  }
	  PyObject_DelItemString(attributes,name);
	  return 0;
  }
	  

  if (PyString_Check(value)) {
	  int len_dims = 1;
	  NrmQuark *qval = malloc(sizeof(NrmQuark));
	  qval[0] = NrmStringToQuark(PyString_AsString(value));
	  md = _NclCreateMultiDVal(NULL,NULL,Ncl_MultiDValData,0,
				   (void*)qval,NULL,1,&len_dims,
				   TEMPORARY,NULL,(NclTypeClass)nclTypestringClass);
  }
  else {
	  int n_dims;
	  int dim_sizes = 1;
	  NrmQuark qtype;
	  int pyarray_type = PyArray_NOTYPE;
	  PyArrayObject *tmparray = (PyArrayObject *)PyDict_GetItemString(attributes,name);
	  if (tmparray != NULL) {
		  pyarray_type = tmparray->descr->type_num;
	  }
	  array = (PyArrayObject *)PyArray_ContiguousFromObject(value, pyarray_type, 0, 1);
	  n_dims = (array->nd == 0) ? 1 : array->nd;
	  qtype = nio_type_from_code(array->descr->type);
	  md = _NclCreateMultiDVal(NULL,NULL,Ncl_MultiDValData,0,
				   (void*)array->data,NULL,n_dims,
				   array->nd == 0 ? &dim_sizes : array->dimensions,
				   TEMPORARY,NULL,_NclNameToTypeClass(qtype));
  }
  if (! md) {
	  nio_seterror();
	  return -1;
  }
  
  if (varid == NC_GLOBAL) {
	  ret = _NclFileWriteAtt(nfile,NrmStringToQuark(name),md,NULL);
  }
  else {
	  ret = _NclFileWriteVarAtt(nfile,nfile->file.var_info[varid]->var_name_quark,
			      NrmStringToQuark(name),md,NULL);
  }
  if (ret > NhlFATAL) {
	  if (PyString_Check(value)) {
		  PyDict_SetItemString(attributes, name, value);
	  }
	  else if (array) {
		  PyDict_SetItemString(attributes, name, (PyObject *)array);
	  }
  }
  return 0;
}

static int
check_if_open(PyNIOFileObject *file, int mode)
{
  /* mode: -1 read, 1 write, 0 other */
  if (file->open) {
    if (mode != 1 || file->write) {
      return 1;
    }
    else {
      PyErr_SetString(NIOError, "write access to read-only file");
      return 0;
    }
  }
  else {
    PyErr_SetString(NIOError, "file has been closed");
    return 0;
  }
}

/*
 * NIOFile object
 * (type declaration in niomodule.h)
 */


/* Destroy file object */

static void
PyNIOFileObject_dealloc(PyNIOFileObject *self)
{
/*
  if (self->open)
    PyNIOFile_Close(self);
*/
  Py_XDECREF(self->dimensions);
  Py_XDECREF(self->variables);
  Py_XDECREF(self->attributes);
  Py_XDECREF(self->name);
  Py_XDECREF(self->mode);
  PyMem_DEL(self);
}

/* Create file object */

PyNIOFileObject *
PyNIOFile_Open(char *filename, char *mode)
{
  PyNIOFileObject *self = PyObject_NEW(PyNIOFileObject,
					  &PyNIOFile_Type);
  NclFile file = NULL;
  int crw;
  struct stat buf;

  if (self == NULL)
    return NULL;
  self->dimensions = NULL;
  self->variables = NULL;
  self->attributes = NULL;
  self->name = NULL;
  self->mode = NULL;
  switch (mode[0]) {
  case 'c':
	  crw = -1;
	  break;
  case 'r':
	  if (strlen(mode) > 1 && (mode[1] == '+' || mode[1] == 'w'))
		  crw = 0;
	  else
		  crw = 1;
	  break;
  case 'w':
	  if (stat(filename,&buf) < 0)
		  crw = -1;
	  else
		  crw = 0;
	  break;
  default:
	  PyErr_SetString(PyExc_IOError, "illegal mode specification");
	  PyNIOFileObject_dealloc(self);
	  return NULL;
  }
  self->open = 0;
  file = _NclCreateFile(NULL,NULL,Ncl_File,0,TEMPORARY,
			NrmStringToQuark(filename),crw);
  if (file) {
	  self->id = (int) file;
	  self->define = 1;
	  self->open = 1;
	  self->write = (crw != 1);
	  nio_file_init(self); 
  }
  else {
	  PyNIOFileObject_dealloc(self);
	  return NULL;
  }
  self->name = PyString_FromString(filename);
  self->mode = PyString_FromString(mode);
  return self;
}

/* Create variables from file */

static int
nio_file_init(PyNIOFileObject *self)
{
  NclFile file = (NclFile) self->id;
  int ndims, nvars, ngattrs;
  int i,j;
  self->dimensions = PyDict_New();
  self->variables = PyDict_New();
  self->attributes = PyDict_New();
  ndims = file->file.n_file_dims;
  nvars = file->file.n_vars;
  ngattrs = file->file.n_file_atts;
  self->recdim = -1; /* for now */
  for (i = 0; i < ndims; i++) {
    char *name;
    long size;
    PyObject *size_ob;
    NclFDimRec *fdim = file->file.file_dim_info[i];
    name = NrmQuarkToString(fdim->dim_name_quark);
    size = fdim->dim_size;
    size_ob = PyInt_FromLong(size);
    PyDict_SetItemString(self->dimensions, name, size_ob);
    Py_DECREF(size_ob);
  }
  for (i = 0; i < nvars; i++) {
    char *name;
    NclBasicDataTypes datatype;
    NclFVarRec *fvar;
    NclFileAttInfoList *att_list;
    int ndimensions, nattrs;
    int *dimids;
    PyNIOVariableObject *variable;
    fvar = file->file.var_info[i];
    ndimensions = fvar->num_dimensions;
    datatype = fvar->data_type;
    nattrs = 0;
    att_list = file->file.var_att_info[i];
    while (att_list != NULL) {
	    nattrs++;
	    att_list = att_list->next;
    }
    name = NrmQuarkToString(fvar->var_name_quark);
    if (ndimensions > 0) {
      dimids = (int *)malloc(ndimensions*sizeof(int));
      if (dimids == NULL) {
	PyErr_NoMemory();
	return 0;
      }
      for (j = 0; j < ndimensions; j++)
	      dimids[j] = fvar->file_dim_num[j];
    }
    else
      dimids = NULL;
    variable = nio_variable_new(self, name, i, data_type(datatype),
				   ndimensions, dimids, nattrs);
    PyDict_SetItemString(self->variables, name, (PyObject *)variable);
    Py_DECREF(variable);
  }

  collect_attributes(self->id, NC_GLOBAL, self->attributes, ngattrs);

  return 1;
}


/* Create dimension */

int
PyNIOFile_CreateDimension(PyNIOFileObject *file, char *name, long size)
{
  PyObject *size_ob;
  NrmQuark qname;
  if (check_if_open(file, 1)) {
	  NclFile nfile = (NclFile) file->id;
	  NhlErrorTypes ret;
	  
	  if (PyDict_GetItemString(file->dimensions,name)) {
		  printf("dimension %s exists: cannot create\n",name);
		  return 0;
	  }
	  if (size == 0 && file->recdim != -1) {
		  PyErr_SetString(NIOError, "there is already an unlimited dimension");
		  return -1;
	  }
	  define_mode(file, 1);
	  qname = NrmStringToQuark(name);
	  ret = _NclFileAddDim(nfile,qname,(int)size,(size == 0 ? 1 : 0));
	  if (ret > NhlWARNING) {
		  if (size == 0) {
			  NclFile nfile = (NclFile) file->id;
			  PyDict_SetItemString(file->dimensions, name, Py_None);
			  file->recdim = _NclFileIsDim(nfile,qname);
		  }
		  else {
			  size_ob = PyInt_FromLong(size);
			  PyDict_SetItemString(file->dimensions, name, size_ob);
			  Py_DECREF(size_ob);
		  }
	  }
	  return 0;
  }
  else
	  return -1;
}

static PyObject *
PyNIOFileObject_new_dimension(PyNIOFileObject *self, PyObject *args)
{
  char *name;
  PyObject *size_ob;
  long size;
  if (!PyArg_ParseTuple(args, "sO", &name, &size_ob))
    return NULL;
  if (size_ob == Py_None)
    size = 0;
  else if (PyInt_Check(size_ob))
    size = PyInt_AsLong(size_ob);
  else {
    PyErr_SetString(PyExc_TypeError, "size must be None or integer");
    return NULL;
  }
  if (PyNIOFile_CreateDimension(self, name, size) == 0) {
    Py_INCREF(Py_None);
    return Py_None;
  }
  else
    return NULL;
}

/* Create variable */

PyNIOVariableObject *
PyNIOFile_CreateVariable( PyNIOFileObject *file, char *name, 
			  int typecode, char **dimension_names, int ndim)
{

  if (check_if_open(file, 1)) {
	  PyNIOVariableObject *variable;
	  int i,id;
	  NclFile nfile = (NclFile) file->id;
	  int *dimids = NULL;
	  NrmQuark *qdims = NULL; 
	  NhlErrorTypes ret;
	  NrmQuark qvar;
	  NrmQuark qtype;
	  define_mode(file, 1);

	  variable = (PyNIOVariableObject *) PyDict_GetItemString(file->variables,name);
	  if (variable) {
		  printf("variable %s exists: cannot create\n",name);
		  return variable;
	  }
		  
	  if (ndim > 0) {
		  qdims = (NrmQuark *)malloc(ndim*sizeof(NrmQuark));
		  dimids = (int *) malloc(ndim*sizeof(NrmQuark));
	  }

	  if (! (qdims && dimids)) {
		  return (PyNIOVariableObject *)PyErr_NoMemory();
	  }
	  for (i = 0; i < ndim; i++) {
		  qdims[i] = NrmStringToQuark(dimension_names[i]);
		  dimids[i] = _NclFileIsDim(nfile,qdims[i]);
                  /*
		  for (j = 0; j < nfile->file.n_file_dims; j++) {
			  dimids[i] = -1;
			  if (nfile->file.file_dim_info[j]->dim_name_quark != qdims[i])
				  continue;
			  dimids[i] = j;
			  break;
		  }
		  */
		  if (dimids[i] == -1) {
			  nio_seterror();
			  if (qdims != NULL) 
				  free(qdims);
			  if (dimids != NULL)
				  free(dimids);
			  return NULL;
		  }
	  }
	  qtype = nio_type_from_code(typecode);
	  qvar = NrmStringToQuark(name);
	  ret = _NclFileAddVar(nfile,qvar,qtype,ndim,qdims);
	  if (ret > NhlWARNING) {
		  id = _NclFileIsVar(nfile,qvar);
		  variable = nio_variable_new(file, name, id, 
					      data_type(nfile->file.var_info[id]->data_type),
					      ndim, dimids, 0);
		  PyDict_SetItemString(file->variables, name, (PyObject *)variable);
		  if (qdims != NULL) 
			  free(qdims);
		  return variable;
	  }
	  else {
		  if (qdims != NULL) 
			  free(qdims);
		  if (dimids != NULL)
			  free(dimids);
		  return NULL;
	  }
  }
  else {
	  return NULL;
  }
}

static PyObject *
PyNIOFileObject_new_variable(PyNIOFileObject *self, PyObject *args)
{
  PyNIOVariableObject *var;
  char **dimension_names;
  PyObject *item, *dim;
  char *name;
  int ndim;
  char type;
  int i;
  if (!PyArg_ParseTuple(args, "scO!", &name, &type, &PyTuple_Type, &dim))
    return NULL;
  ndim = PyTuple_Size(dim);
  if (ndim == 0)
    dimension_names = NULL;
  else {
    dimension_names = (char **)malloc(ndim*sizeof(char *));
    if (dimension_names == NULL) {
      PyErr_SetString(PyExc_MemoryError, "out of memory");
      return NULL;
    }
  }
  for (i = 0; i < ndim; i++) {
    item = PyTuple_GetItem(dim, i);
    if (PyString_Check(item))
      dimension_names[i] = PyString_AsString(item);
    else {
      PyErr_SetString(PyExc_TypeError, "dimension name must be a string");
      free(dimension_names);
      return NULL;
    }
  }
  var = PyNIOFile_CreateVariable(self, name, type, dimension_names, ndim);
  free(dimension_names);
  return (PyObject *)var;
}

/* Return a variable object referring to an existing variable */

static PyNIOVariableObject *
PyNIOFile_GetVariable(PyNIOFileObject *file, char *name)
{
  return (PyNIOVariableObject *)PyDict_GetItemString(file->variables, name);
}

/* Synchronize output */

#if 0
int
PyNIOFile_Sync(PyNIOFileObject *file)
{
  if (check_if_open(file, 0)) {
#if 0
    define_mode(file, 0);
    if (ncsync(file->id) == -1) {
      nio_seterror();
      return -1;
    }
    else
#endif
      return 0;
  }
  else
    return -1;
}


static PyObject *
PyNIOFileObject_sync(PyNIOFileObject *self, PyObject *args)
{
  if (!PyArg_ParseTuple(args, ""))
    return NULL;
  if (PyNIOFile_Sync(self) == 0) {
    Py_INCREF(Py_None);
    return Py_None;
  }
  else
    return NULL;
}
#endif

/* Close file */

int
PyNIOFile_Close(PyNIOFileObject *file)
{
  if (check_if_open(file, 0)) {
	  _NclDestroyObj((NclObj)file->id);
	  file->open = 0;
	  return 0;
  }
  else
	  return -1;
}

static PyObject *
PyNIOFileObject_close(PyNIOFileObject *self, PyObject *args)
{
  char *history = NULL;
  if (!PyArg_ParseTuple(args, "|s", &history))
    return NULL;
  if (history != NULL)
    PyNIOFile_AddHistoryLine(self, history);
  if (PyNIOFile_Close(self) == 0) {
    Py_INCREF(Py_None);
    return Py_None;
  }
  else
    return NULL;
}



/* Method table */

static PyMethodDef PyNIOFileObject_methods[] = {
  {"close", (PyCFunction)PyNIOFileObject_close, 1},
/* {"sync", (PyCFunction)PyNIOFileObject_sync, 1}, */
  {"create_dimension", (PyCFunction)PyNIOFileObject_new_dimension, 1},
  {"create_variable", (PyCFunction)PyNIOFileObject_new_variable, 1},
  {NULL, NULL}		/* sentinel */
};


/* Attribute access */

PyObject *
PyNIOFile_GetAttribute(PyNIOFileObject *self, char *name)
{
  PyObject *value;
  if (check_if_open(self, -1)) {
    if (strcmp(name, "dimensions") == 0) {
      Py_INCREF(self->dimensions);
      return self->dimensions;
    }
    if (strcmp(name, "variables") == 0) {
      Py_INCREF(self->variables);
      return self->variables;
    }
    if (strcmp(name, "__dict__") == 0) {
      Py_INCREF(self->attributes);
      return self->attributes;
    }
    value = PyDict_GetItemString(self->attributes, name);
    if (value != NULL) {
      Py_INCREF(value);
      return value;
    }
    else {
        PyErr_Clear();
      return Py_FindMethod(PyNIOFileObject_methods, (PyObject *)self, name);
    }
  }
  else
    return NULL;
}

int
PyNIOFile_SetAttribute(PyNIOFileObject *self, char *name, PyObject *value)
{
  if (check_if_open(self, 1)) {
    if (strcmp(name, "dimensions") == 0 ||
	strcmp(name, "variables") == 0 ||
	strcmp(name, "__dict__") == 0) {
      PyErr_SetString(PyExc_TypeError, "object has read-only attributes");
      return -1;
    }
    define_mode(self, 1);
    return set_attribute(self, NC_GLOBAL, self->attributes, name, value);
  }
  else
    return -1;
}
int
PyNIOFile_SetAttributeString(PyNIOFileObject *self, char *name, char *value)
{
  PyObject *string = PyString_FromString(value);
  if (string != NULL)
    return PyNIOFile_SetAttribute(self, name, string);
  else
    return -1;
}

int
PyNIOFile_AddHistoryLine(PyNIOFileObject *self, char *text)
{
  static char *history = "history";
  int alloc, old, new, new_alloc;
  PyStringObject *new_string;
  PyObject *h = PyNIOFile_GetAttribute(self, history);
  if (h == NULL) {
    PyErr_Clear();
    alloc = 0;
    old = 0;
    new = strlen(text);
  }
  else {
    alloc = PyString_Size(h);
    old = strlen(PyString_AsString(h));
    new = old + strlen(text) + 1;
  }
  new_alloc = (new <= alloc) ? alloc : new + 500;
  new_string = (PyStringObject *)PyString_FromStringAndSize(NULL, new_alloc);
  if (new_string) {
    char *s = new_string->ob_sval;
    int len, ret;
    memset(s, 0, new_alloc+1);
    if (h == NULL)
      len = -1;
    else {
      strcpy(s, PyString_AsString(h));
      len = strlen(s);
      s[len] = '\n';
    }
    strcpy(s+len+1, text);
    ret = PyNIOFile_SetAttribute(self, history, (PyObject *)new_string);
    Py_XDECREF(h);
    Py_DECREF(new_string);
    return ret;
  }
  else
    return -1;
}

/* Printed representation */
static PyObject *
PyNIOFileObject_repr(PyNIOFileObject *file)
{
  char buf[300];
  sprintf(buf, "<%s Nio file '%.256s', mode '%.10s' at %lx>",
	  file->open ? "open" : "closed",
	  PyString_AsString(file->name),
	  PyString_AsString(file->mode),
	  (long)file);
  return PyString_FromString(buf);
}

/* Type definition */

statichere PyTypeObject PyNIOFile_Type = {
  PyObject_HEAD_INIT(NULL)
  0,		/*ob_size*/
  "NIOFile",	/*tp_name*/
  sizeof(PyNIOFileObject),	/*tp_basicsize*/
  0,		/*tp_itemsize*/
  /* methods */
  (destructor)PyNIOFileObject_dealloc, /*tp_dealloc*/
  0,			/*tp_print*/
  (getattrfunc)PyNIOFile_GetAttribute, /*tp_getattr*/
  (setattrfunc)PyNIOFile_SetAttribute, /*tp_setattr*/
  0,			/*tp_compare*/
  (reprfunc)PyNIOFileObject_repr,   /*tp_repr*/
  0,			/*tp_as_number*/
  0,			/*tp_as_sequence*/
  0,			/*tp_as_mapping*/
  0,			/*tp_hash*/
};

/*
 * NIOVariable object
 * (type declaration in niomodule.h)
 */

/* Destroy variable object */

static void
PyNIOVariableObject_dealloc(PyNIOVariableObject *self)
{
  if (self->dimids != NULL)
    free(self->dimids);
  if (self->name != NULL)
    free(self->name);
  Py_XDECREF(self->file);
  PyMem_DEL(self);
}

/* Create variable object */

statichere PyNIOVariableObject *
nio_variable_new(PyNIOFileObject *file, char *name, int id, 
		 int type, int ndims, int *dimids, int nattrs)
{
  PyNIOVariableObject *self;
  NclFile nfile = (NclFile) file->id;
  NclFVarRec *fvar;
  int i;
  if (check_if_open(file, -1)) {
    self = PyObject_NEW(PyNIOVariableObject, &PyNIOVariable_Type);
    if (self == NULL)
      return NULL;
    self->file = file;
    Py_INCREF(file);
    self->id = id;
    self->type = type;
    self->nd = ndims;
    self->dimids = dimids;
    self->unlimited = 0;
    fvar = nfile->file.var_info[id];
    if (ndims > 0) {
	    self->dimensions = (long *)malloc(ndims*sizeof(long));
	    if (self->dimensions != NULL) {
		    for (i = 0; i < ndims; i++) {
			    self->dimensions[i] = nfile->file.file_dim_info[dimids[i]]->dim_size;
			    if (nfile->file.file_dim_info[dimids[i]]->is_unlimited)
				    self->unlimited = 1;
		    }
	    }
    }
    self->name = (char *)malloc(strlen(name)+1);
    if (self->name != NULL)
      strcpy(self->name, name);
    self->attributes = PyDict_New();
    collect_attributes(file->id, self->id, self->attributes, nattrs);
    return self;
  }
  else
    return NULL;
}

/* Return value */

static PyObject *
PyNIOVariableObject_value(PyNIOVariableObject *self, PyObject *args)
{
  PyNIOIndex *indices;
  if (!PyArg_ParseTuple(args, ""))
    return NULL;
  if (self->nd == 0)
    indices = NULL;
  else
    indices = PyNIOVariable_Indices(self);
  return PyArray_Return(PyNIOVariable_ReadAsArray(self, indices));
}

/* Assign value */

static PyObject *
PyNIOVariableObject_assign(PyNIOVariableObject *self, PyObject *args)
{
  PyObject *value;
  PyNIOIndex *indices;
  if (!PyArg_ParseTuple(args, "O", &value))
    return NULL;
  if (self->nd == 0)
    indices = NULL;
  else
    indices = PyNIOVariable_Indices(self);
  PyNIOVariable_WriteArray(self, indices, value);
  Py_INCREF(Py_None);
  return Py_None;
}

/* Return typecode */

static PyObject *
PyNIOVariableObject_typecode(PyNIOVariableObject *self, PyObject *args)
{
  char t;
  if (!PyArg_ParseTuple(args, ""))
    return NULL;
  t = typecode(self->type);
  return PyString_FromStringAndSize(&t, 1);
}

/* Method table */

static PyMethodDef PyNIOVariableObject_methods[] = {
  {"assign_value", (PyCFunction)PyNIOVariableObject_assign, 1},
  {"get_value", (PyCFunction)PyNIOVariableObject_value, 1},
  {"typecode", (PyCFunction)PyNIOVariableObject_typecode, 1},
  {NULL, NULL}		/* sentinel */
};

/* Attribute access */

static int
PyNIOVariable_GetRank( PyNIOVariableObject *var)
{
  return var->nd;
}

static size_t *
PyNIOVariable_GetShape(PyNIOVariableObject *var)
{
  int i;
  if (check_if_open(var->file, -1)) {
	  NclFile nfile = (NclFile) var->file->id;
	  for (i = 0; i < var->nd; i++) {
		  var->dimensions[i] = nfile->file.file_dim_info[var->dimids[i]]->dim_size;
	  }
	  return var->dimensions;
  }
  else
	  return NULL;
}

static PyObject *
PyNIOVariable_GetAttribute(PyNIOVariableObject *self, char *name)
{
  PyObject *value;
  if (strcmp(name, "shape") == 0) {
    PyObject *tuple;
    int i;
    if (check_if_open(self->file, -1)) {
      PyNIOVariable_GetShape(self);
      tuple = PyTuple_New(self->nd);
      for (i = 0; i < self->nd; i++)
	PyTuple_SetItem(tuple, i, PyInt_FromLong(self->dimensions[i]));
      return tuple;
    }
    else
      return NULL;
  }
  if (strcmp(name, "dimensions") == 0) {
    PyObject *tuple;
    char *name;
    int i;
    if (check_if_open(self->file, -1)) {
      tuple = PyTuple_New(self->nd);
      NclFile nfile = (NclFile) self->file->id;
      for (i = 0; i < self->nd; i++) {
	      name = NrmQuarkToString(nfile->file.file_dim_info[self->dimids[i]]->dim_name_quark);
	      PyTuple_SetItem(tuple, i, PyString_FromString(name));
      }
      return tuple;
    }
    else
      return NULL;
  }
  if (strcmp(name, "__dict__") == 0) {
    Py_INCREF(self->attributes);
    return self->attributes;
  }
  value = PyDict_GetItemString(self->attributes, name);
  if (value != NULL) {
    Py_INCREF(value);
    return value;
  }
  else {
    PyErr_Clear();
    return Py_FindMethod(PyNIOVariableObject_methods, (PyObject *)self, name);
  }
}

static int
PyNIOVariable_SetAttribute(PyNIOVariableObject *self, char *name, PyObject *value)
{
  if (check_if_open(self->file, 1)) {
    if (strcmp(name, "shape") == 0 ||
	strcmp(name, "dimensions") == 0 ||
	strcmp(name, "__dict__") == 0) {
      PyErr_SetString(PyExc_TypeError, "object has read-only attributes");
      return -1;
    }
    define_mode(self->file, 1);
    return set_attribute(self->file, self->id, self->attributes,
			 name, value);
  }
  else
    return -1;
}

int
PyNIOVariable_SetAttributeString(PyNIOVariableObject *self, char *name, char *value)
{
  PyObject *string = PyString_FromString(value);
  if (string != NULL)
    return PyNIOVariable_SetAttribute(self, name, string);
  else
    return -1;
}


/* Subscripting */

static int
PyNIOVariableObject_length(PyNIOVariableObject *self)
{
  if (self->nd > 0)
    return self->dimensions[0];
  else
    return 0;
}

PyNIOIndex *
PyNIOVariable_Indices(PyNIOVariableObject *var)
{
  PyNIOIndex *indices = 
    (PyNIOIndex *)malloc(var->nd*sizeof(PyNIOIndex));
  int i;
  if (indices != NULL)
    for (i = 0; i < var->nd; i++) {
      indices[i].start = 0;
      indices[i].stop = var->dimensions[i];
      indices[i].stride = 1;
      indices[i].item = 0;
    }
  else
    PyErr_SetString(PyExc_MemoryError, "out of memory");
  return indices;
}

PyArrayObject *
PyNIOVariable_ReadAsArray(PyNIOVariableObject *self,PyNIOIndex *indices)
{
  int *dims;
  PyArrayObject *array;
  int i, d;
  int nitems;
  int error = 0;
  d = 0;
  nitems = 1;
  if (!check_if_open(self->file, -1)) {
    free(indices);
    return NULL;
  }
  define_mode(self->file, 0);
  if (self->nd == 0)
    dims = NULL;
  else {
    dims = (int *)malloc(self->nd*sizeof(int));
    if (dims == NULL) {
      free(indices);
      return (PyArrayObject *)PyErr_NoMemory();
    }
  }
  /* convert from Python to NCL indexing */
  /* negative stride in Python requires the start index to be greater than
     the end index: in NCL negative stride reverses the direction 
     implied by the index start and stop.
  */
  for (i = 0; i < self->nd; i++) {
    error = error || (indices[i].stride == 0);
    if (indices[i].stride < 0) {
	    indices[i].stop += 1;
	    indices[i].stride = -indices[i].stride;
    }
    else {
	    indices[i].stop -= 1;
    }
    if (indices[i].start < 0)
      indices[i].start += self->dimensions[i];
    if (indices[i].start < 0)
      indices[i].start = 0;
    if (indices[i].start > self->dimensions[i] -1)
      indices[i].start = self->dimensions[i] -1;
    if (indices[i].item != 0)
      indices[i].stop = indices[i].start;
    else {
      if (indices[i].stop < 0)
	indices[i].stop += self->dimensions[i];
      if (indices[i].stop < 0)
	indices[i].stop = 0;
      if (indices[i].stop > self->dimensions[i] -1)
	indices[i].stop = self->dimensions[i] -1;
      dims[d] = abs((indices[i].stop-indices[i].start)/indices[i].stride)+1;
      if (dims[d] < 0)
	dims[d] = 0;
      nitems *= dims[d];
      d++;
    }
  }
  if (error) {
    PyErr_SetString(PyExc_IndexError, "illegal index");
    if (dims != NULL)
      free(dims);
    if (indices != NULL)
      free(indices);
    return NULL;
  }
  array = (PyArrayObject *)PyArray_FromDims(d, dims, self->type);
  if (array != NULL && nitems > 0) {
    if (self->nd == 0) {
	    NclFile nfile = (NclFile) self->file->id;
	    NclMultiDValData md = _NclFileReadVarValue
		    (nfile,NrmStringToQuark(self->name),NULL);
	    if (! md) {
		    nio_seterror();
		    Py_DECREF(array);
		    array = NULL;
	    }
	    /* all we care about is the actual value */
	    array->data = md->multidval.val;
	    md->multidval.val = NULL;
	    _NclDestroyObj((NclObj)md);
    }
    else {
	    NclSelectionRecord *sel_ptr;
	    sel_ptr = (NclSelectionRecord*)malloc(sizeof(NclSelectionRecord));
	    if (sel_ptr != NULL) {
		    NclFile nfile = (NclFile) self->file->id;
		    NclMultiDValData md;
		    sel_ptr->n_entries = self->nd;
		    for (i = 0; i < self->nd; i++) {
			    sel_ptr->selection[i].sel_type = Ncl_SUBSCR;
			    sel_ptr->selection[i].dim_num = i;
			    sel_ptr->selection[i].u.sub.start = indices[i].start;
			    sel_ptr->selection[i].u.sub.finish = indices[i].stop;
			    sel_ptr->selection[i].u.sub.stride = indices[i].stride;
			    sel_ptr->selection[i].u.sub.is_single = 
				    indices[i].item != 0;
		    }
		    md = _NclFileReadVarValue
			    (nfile,NrmStringToQuark(self->name),sel_ptr);
		    if (! md) {
			    nio_seterror();
			    Py_DECREF(array);
			    array = NULL;
		    }
		    
		    /* all we care about is the actual value */
		    array->data = md->multidval.val;
		    md->multidval.val = NULL;
		    _NclDestroyObj((NclObj)md);
		    free(sel_ptr);
	    }
    }
  }
  free(dims);
  free(indices);
  return array;
}

static PyStringObject *
PyNIOVariable_ReadAsString(PyNIOVariableObject *self)
{
  if (self->type != PyArray_CHAR || self->nd != 1) {
    PyErr_SetString(NIOError, "not a string variable");
    return NULL;
  }
  if (check_if_open(self->file, -1)) {
	  char *tstr;
	  PyObject *string;
	  NclFile nfile = (NclFile) self->file->id;
	  NclMultiDValData md = _NclFileReadVarValue
		  (nfile,NrmStringToQuark(self->name),NULL);
	  if (! md) {
		  nio_seterror();
		  return NULL;
	  }
	  /* all we care about is the actual value */
	  tstr = NrmQuarkToString(*(NrmQuark *) md->multidval.val);
	  _NclDestroyObj((NclObj)md);
	  string = PyString_FromString(tstr);
	  return (PyStringObject *)string;
  }
  else
    return NULL;
}

static int
PyNIOVariable_WriteArray(PyNIOVariableObject *self, PyNIOIndex *indices, PyObject *value)
{
  int *dims;
  PyArrayObject *array;
  int i, d;
  int nitems,var_el_count;
  int error = 0;
  int ret = 0;

  /* update shape */
  (void) PyNIOVariable_GetShape(self);
  d = 0;
  nitems = 1;
  var_el_count = 1;
  if (!check_if_open(self->file, 1)) {
    free(indices);
    return -1;
  }
  if (self->nd == 0)
    dims = NULL;
  else {
    dims = (int *)malloc(self->nd*sizeof(int));
    if (dims == NULL) {
      free(indices);
      PyErr_SetString(PyExc_MemoryError, "out of memory");
      return -1;
    }
  }
  define_mode(self->file, 0);
  /* convert from Python to NCL indexing */
  /* negative stride in Python requires the start index to be greater than
     the end index: in NCL negative stride reverses the direction 
     implied by the index start and stop.
  */

  for (i = 0; i < self->nd; i++) {
	  var_el_count *= self->dimensions[i];
	  error = error || (indices[i].stride == 0);
	  if (indices[i].stride < 0) {
		  indices[i].stop += 1;
		  indices[i].stride = -indices[i].stride;
	  }
	  else {
		  indices[i].stop -= 1;
	  }
	  if (indices[i].start < 0)
		  indices[i].start += self->dimensions[i];
	  if (indices[i].start < 0)
		  indices[i].start = 0;
	  if (indices[i].stop < 0)
		  indices[i].stop += self->dimensions[i];
	  if (indices[i].stop < 0)
		  indices[i].stop = 0;
	  if (i > 0 || !self->unlimited) {
		  if (indices[i].start > self->dimensions[i] -1)
			  indices[i].start = self->dimensions[i] - 1;
		  if (indices[i].stop > self->dimensions[i] - 1)
			  indices[i].stop = self->dimensions[i] - 1;
	  }
	  if (indices[i].item == 0) {
		  dims[d] = abs((indices[i].stop-indices[i].start)/indices[i].stride)+1;
		  if (dims[d] < 0)
			  dims[d] = 0;
		  nitems *= dims[d];
		  d++;
	  }
	  else
		  indices[i].stop = indices[i].start;
  }
  if (error) {
    PyErr_SetString(PyExc_IndexError, "illegal index");
    if (dims != NULL)
      free(dims);
    if (indices != NULL)
      free(indices);
    return -1;
  }
  array = (PyArrayObject *)PyArray_ContiguousFromObject(value,self->type,0,d);
  if (array != NULL) {
	  int n_dims;
	  int scalar_size = 1;
	  NclFile nfile = (NclFile) self->file->id;
	  NclMultiDValData md;
	  NhlErrorTypes nret;
	  int select_all = 1;

	  n_dims = array->nd;
	  if (array->nd == 0) {
		  n_dims = 1;
	  }
	  /*
	  if (array->nd != self->nd)
		  select_all = 0;
	  else {
		  for (i = 0; i < self->nd; i++) {
			  if (dims[i] == array->dimensions[i])
				  continue;
			  select_all = 0;
			  break;
		  }
	  }
	  */
	  if (nitems < var_el_count || self->unlimited)
		  select_all = 0;
	  NrmQuark qtype = nio_type_from_code(array->descr->type);
	  md = _NclCreateMultiDVal(NULL,NULL,Ncl_MultiDValData,0,
				   (void*)array->data,NULL,n_dims,
				   array->nd == 0 ? &scalar_size : array->dimensions,
				   TEMPORARY,NULL,_NclNameToTypeClass(qtype));
	  if (! md) {
		  nret = NhlFATAL;
	  }
	  else if (select_all) {
		  nret = _NclFileWriteVar(nfile,NrmStringToQuark(self->name),md,NULL);
	  }
	  else {
		  NclSelectionRecord *sel_ptr;
		  sel_ptr = (NclSelectionRecord*)malloc(sizeof(NclSelectionRecord));
		  if (sel_ptr == NULL) {
			  nret = NhlFATAL;
		  }
		  else {
			  sel_ptr->n_entries = self->nd;
			  for (i = 0; i < self->nd; i++) {
				  sel_ptr->selection[i].sel_type = Ncl_SUBSCR;
				  sel_ptr->selection[i].dim_num = i;
				  sel_ptr->selection[i].u.sub.start = indices[i].start;
				  sel_ptr->selection[i].u.sub.finish = indices[i].stop;
				  sel_ptr->selection[i].u.sub.stride = indices[i].stride;
				  sel_ptr->selection[i].u.sub.is_single = 
					  indices[i].item != 0;
			  }
			  nret = _NclFileWriteVar(nfile,NrmStringToQuark(self->name),md,sel_ptr);
			  free(sel_ptr);
		  }
	  }
	  Py_DECREF(array);
	  if (nret < NhlWARNING)
		  ret = -1;
  }
  /* update shape */
  (void) PyNIOVariable_GetShape(self);
  free(dims);
  free(indices);
  return ret;
}


static int
PyNIOVariable_WriteString(PyNIOVariableObject *self, PyStringObject *value)
{
  long len;

  if (self->type != PyArray_CHAR || self->nd != 1) {
	  PyErr_SetString(NIOError, "not a string variable");
	  return -1;
  }
  len = PyString_Size((PyObject *)value);
  if (len > self->dimensions[0]) {
	  PyErr_SetString(PyExc_ValueError, "string too long");
	  return -1;
  }
  if (self->dimensions[0] > len)
	  len++;
  if (check_if_open(self->file, 1)) {
	  NclFile nfile = (NclFile) self->file->id;
	  NclMultiDValData md;
	  NhlErrorTypes nret;
	  int str_dim_size = 1;
	  NrmQuark qstr = NrmStringToQuark(PyString_AsString((PyObject *)value));
	  define_mode(self->file, 0);
	  md = _NclCreateMultiDVal(NULL,NULL,Ncl_MultiDValData,0,
				   (void*)&qstr,NULL,1,
				   &str_dim_size,
				   TEMPORARY,NULL,_NclNameToTypeClass(NrmStringToQuark("string")));
	  if (! md) {
		  nret = NhlFATAL;
	  }
	  else {
		  nret = _NclFileWriteVar(nfile,NrmStringToQuark(self->name),md,NULL);
	  }
	  if (nret < NhlWARNING) {
		  nio_seterror();
		  return -1;
	  }
	  return 0;
  }
  else {
	  return -1;
  }
}


static PyObject *
PyNIOVariableObject_item(PyNIOVariableObject *self, int i)
{
  PyNIOIndex *indices;
  if (self->nd == 0) {
    PyErr_SetString(PyExc_TypeError, "Not a sequence");
    return NULL;
  }
  indices = PyNIOVariable_Indices(self);
  if (indices != NULL) {
    indices[0].start = i;
    indices[0].stop = i+1;
    indices[0].item = 1;
    return PyArray_Return(PyNIOVariable_ReadAsArray(self, indices));
  }
  return NULL;
}

static PyObject *
PyNIOVariableObject_slice(PyNIOVariableObject *self, int low, int high)
{
  PyNIOIndex *indices;
  if (self->nd == 0) {
    PyErr_SetString(PyExc_TypeError, "Not a sequence");
    return NULL;
  }
  indices = PyNIOVariable_Indices(self);
  if (indices != NULL) {
    indices[0].start = low;
    indices[0].stop = high;
    return PyArray_Return(PyNIOVariable_ReadAsArray(self, indices));
  }
  return NULL;
}

static PyObject *
PyNIOVariableObject_subscript(PyNIOVariableObject *self, PyObject *index)
{
  PyNIOIndex *indices;
  if (PyInt_Check(index)) {
    int i = PyInt_AsLong(index);
    return PyNIOVariableObject_item(self, i);
  }
  if (self->nd == 0) {
    PyErr_SetString(PyExc_TypeError, "Not a sequence");
    return NULL;
  }
  indices = PyNIOVariable_Indices(self);
  if (indices != NULL) {
    if (PySlice_Check(index)) {
      PySlice_GetIndices((PySliceObject *)index, self->dimensions[0],
			 &indices->start, &indices->stop, &indices->stride);
      return PyArray_Return(PyNIOVariable_ReadAsArray(self, indices));
    }
    if (PyTuple_Check(index)) {
      int ni = PyTuple_Size(index);
      if (ni <= self->nd) {
	int i, d;
	d = 0;
	for (i = 0; i < ni; i++) {
	  PyObject *subscript = PyTuple_GetItem(index, i);
	  if (PyInt_Check(subscript)) {
	    int n = PyInt_AsLong(subscript);
	    indices[d].start = n;
	    indices[d].stop = n+1;
	    indices[d].item = 1;
	    d++;
	  }
	  else if (PySlice_Check(subscript)) {
	    PySlice_GetIndices((PySliceObject *)subscript, self->dimensions[d],
			       &indices[d].start, &indices[d].stop,
			       &indices[d].stride);
	    d++;
	  }
	  else if (subscript == Py_Ellipsis) {
	    d = self->nd - ni + i + 1;
	  }
	  else {
	    PyErr_SetString(PyExc_TypeError, "illegal subscript type");
	    free(indices);
	    return NULL;
	  }
	}
	return PyArray_Return(PyNIOVariable_ReadAsArray(self, indices));
      }
      else {
	PyErr_SetString(PyExc_IndexError, "too many subscripts");
	free(indices);
	return NULL;
      }
    }
    PyErr_SetString(PyExc_TypeError, "illegal subscript type");
    free(indices);
  }
  return NULL;
}

static int
PyNIOVariableObject_ass_item(PyNIOVariableObject *self, int i, PyObject *value)
{
  PyNIOIndex *indices;
  if (value == NULL) {
    PyErr_SetString(PyExc_ValueError, "Can't delete elements.");
    return -1;
  }
  if (self->nd == 0) {
    PyErr_SetString(PyExc_TypeError, "Not a sequence");
    return -1;
  }
  indices = PyNIOVariable_Indices(self);
  if (indices != NULL) {
    indices[0].start = i;
    indices[0].stop = i+1;
    indices[0].item = 1;
    return PyNIOVariable_WriteArray(self, indices, value);
  }
  return -1;
}

static int
PyNIOVariableObject_ass_slice(PyNIOVariableObject *self, int low, int high, PyObject *value)
{
  PyNIOIndex *indices;
  if (value == NULL) {
    PyErr_SetString(PyExc_ValueError, "Can't delete elements.");
    return -1;
  }
  if (self->nd == 0) {
    PyErr_SetString(PyExc_TypeError, "Not a sequence");
    return -1;
  }
  indices = PyNIOVariable_Indices(self);
  if (indices != NULL) {
    indices[0].start = low;
    indices[0].stop = high;
    return PyNIOVariable_WriteArray(self, indices, value);
  }
  return -1;
}

static int
PyNIOVariableObject_ass_subscript(PyNIOVariableObject *self, PyObject *index, PyObject *value)
{
  PyNIOIndex *indices;
  if (PyInt_Check(index)) {
    int i = PyInt_AsLong(index);
    return PyNIOVariableObject_ass_item(self, i, value);
  }
  if (value == NULL) {
    PyErr_SetString(PyExc_ValueError, "Can't delete elements.");
    return -1;
  }
  if (self->nd == 0) {
    PyErr_SetString(PyExc_TypeError, "Not a sequence");
    return -1;
  }
  indices = PyNIOVariable_Indices(self);
  if (indices != NULL) {
    if (PySlice_Check(index)) {
      PySlice_GetIndices((PySliceObject *)index, self->dimensions[0],
			 &indices->start, &indices->stop, &indices->stride);
      return PyNIOVariable_WriteArray(self, indices, value);
    }
    if (PyTuple_Check(index)) {
      int ni = PyTuple_Size(index);
      if (ni <= self->nd) {
	int i, d;
	d = 0;
	for (i = 0; i < ni; i++) {
	  PyObject *subscript = PyTuple_GetItem(index, i);
	  if (PyInt_Check(subscript)) {
	    int n = PyInt_AsLong(subscript);
	    indices[d].start = n;
	    indices[d].stop = n+1;
	    indices[d].item = 1;
	    d++;
	  }
	  else if (PySlice_Check(subscript)) {
	    PySlice_GetIndices((PySliceObject *)subscript, self->dimensions[d],
			       &indices[d].start, &indices[d].stop,
			       &indices[d].stride);
	    d++;
	  }
	  else if (subscript == Py_Ellipsis) {
	    d = self->nd - ni + i + 1;
	  }
	  else {
	    PyErr_SetString(PyExc_TypeError, "illegal subscript type");
	    free(indices);
	    return -1;
	  }
	}
	return PyNIOVariable_WriteArray(self, indices, value);
      }
      else {
	PyErr_SetString(PyExc_IndexError, "too many subscripts");
	free(indices);
	return -1;
      }
    }
    PyErr_SetString(PyExc_TypeError, "illegal subscript type");
    free(indices);
  }
  return -1;
}

/* Type definition */

static PyObject *
PyNIOVariableObject_error1(PyNIOVariableObject *self, PyNIOVariableObject *other)
{
  PyErr_SetString(PyExc_TypeError, "can't add NIO variables");
  return NULL;
}

static PyObject *
PyNIOVariableObject_error2(PyNIOVariableObject *self,  int n)
{
  PyErr_SetString(PyExc_TypeError, "can't multiply NIO variables");
  return NULL;
}


static PySequenceMethods PyNIOVariableObject_as_sequence = {
  (inquiry)PyNIOVariableObject_length,		/*sq_length*/
  (binaryfunc)PyNIOVariableObject_error1,       /*nb_add*/
  (intargfunc)PyNIOVariableObject_error2,       /*nb_multiply*/
  (intargfunc)PyNIOVariableObject_item,		/*sq_item*/
  (intintargfunc)PyNIOVariableObject_slice,	/*sq_slice*/
  (intobjargproc)PyNIOVariableObject_ass_item,	/*sq_ass_item*/
  (intintobjargproc)PyNIOVariableObject_ass_slice,   /*sq_ass_slice*/
};



static PyMappingMethods PyNIOVariableObject_as_mapping = {
  (inquiry)PyNIOVariableObject_length,		/*mp_length*/
  (binaryfunc)PyNIOVariableObject_subscript,	      /*mp_subscript*/
  (objobjargproc)PyNIOVariableObject_ass_subscript,   /*mp_ass_subscript*/
};


statichere PyTypeObject PyNIOVariable_Type = {
  PyObject_HEAD_INIT(NULL)
  0,		     /*ob_size*/
  "NIOVariable",  /*tp_name*/
  sizeof(PyNIOVariableObject),	     /*tp_basicsize*/
  0,		     /*tp_itemsize*/
  /* methods */
  (destructor)PyNIOVariableObject_dealloc, /*tp_dealloc*/
  0,			/*tp_print*/
  (getattrfunc)PyNIOVariable_GetAttribute, /*tp_getattr*/
  (setattrfunc)PyNIOVariable_SetAttribute, /*tp_setattr*/
  0,			/*tp_compare*/
  0,			/*tp_repr*/
  0,			/*tp_as_number*/
  &PyNIOVariableObject_as_sequence,	/*tp_as_sequence*/
  &PyNIOVariableObject_as_mapping,	/*tp_as_mapping*/
  0,0,
  0,			/*tp_hash*/
};


/* Creator for NIOFile objects */

static PyObject *
NIOFile(PyObject *self, PyObject *args)
{
  char *filename;
  char *mode = NULL;
  char *history = NULL;
  PyNIOFileObject *file;

  if (!PyArg_ParseTuple(args, "s|ss:open_file", &filename, &mode, &history))
    return NULL;
  if (mode == NULL)
    mode = "r";
  file = PyNIOFile_Open(filename, mode);
  if (file == NULL) {
    nio_seterror();
    return NULL;
  }
  if (history != NULL)
    PyNIOFile_AddHistoryLine(file, history);
  return (PyObject *)file;
}

/* Table of functions defined in the module */

static PyMethodDef nio_methods[] = {
  {"open_file",	NIOFile, 1},
  {NULL,		NULL}		/* sentinel */
};

/* Module initialization */

void
initNio(void)
{
  PyObject *m, *d;
  static void *PyNIO_API[PyNIO_API_pointers];


  /* Initialize type object headers */
  PyNIOFile_Type.ob_type = &PyType_Type;
  PyNIOVariable_Type.ob_type = &PyType_Type;

  /* Create the module and add the functions */
  m = Py_InitModule("Nio", nio_methods);

  NioInitialize();
  
  /* Import the array module */
#ifdef import_array
  import_array();
#endif

  /* Add error object the module */
  d = PyModule_GetDict(m);
  NIOError = PyString_FromString("NIOError");
  PyDict_SetItemString(d, "NIOError", NIOError);

  /* Initialize C API pointer array and store in module */
  PyNIO_API[PyNIOFile_Type_NUM] = (void *)&PyNIOFile_Type;
  PyNIO_API[PyNIOVariable_Type_NUM] = (void *)&PyNIOVariable_Type;
  PyNIO_API[PyNIOFile_Open_NUM] = (void *)&PyNIOFile_Open;
  PyNIO_API[PyNIOFile_Close_NUM] = (void *)&PyNIOFile_Close;
/*
  PyNIO_API[PyNIOFile_Sync_NUM] = (void *)&PyNIOFile_Sync;
*/
  PyNIO_API[PyNIOFile_CreateDimension_NUM] =
    (void *)&PyNIOFile_CreateDimension;
  PyNIO_API[PyNIOFile_CreateVariable_NUM] =
    (void *)&PyNIOFile_CreateVariable;
  PyNIO_API[PyNIOFile_GetVariable_NUM] =
    (void *)&PyNIOFile_GetVariable;
  PyNIO_API[PyNIOVariable_GetRank_NUM] =
    (void *)&PyNIOVariable_GetRank;
  PyNIO_API[PyNIOVariable_GetShape_NUM] =
    (void *)&PyNIOVariable_GetShape;
  PyNIO_API[PyNIOVariable_Indices_NUM] =
    (void *)&PyNIOVariable_Indices;
  PyNIO_API[PyNIOVariable_ReadAsArray_NUM] =
    (void *)&PyNIOVariable_ReadAsArray;
  PyNIO_API[PyNIOVariable_ReadAsString_NUM] =
    (void *)&PyNIOVariable_ReadAsString;
  PyNIO_API[PyNIOVariable_WriteArray_NUM] =
    (void *)&PyNIOVariable_WriteArray;
  PyNIO_API[PyNIOVariable_WriteString_NUM] =
    (void *)&PyNIOVariable_WriteString;
  PyNIO_API[PyNIOFile_GetAttribute_NUM] =
    (void *)&PyNIOFile_GetAttribute;
  PyNIO_API[PyNIOFile_SetAttribute_NUM] =
    (void *)&PyNIOFile_SetAttribute;
  PyNIO_API[PyNIOFile_SetAttributeString_NUM] =
    (void *)&PyNIOFile_SetAttributeString;
  PyNIO_API[PyNIOVariable_GetAttribute_NUM] =
    (void *)&PyNIOVariable_GetAttribute;
  PyNIO_API[PyNIOVariable_SetAttribute_NUM] =
    (void *)&PyNIOVariable_SetAttribute;
  PyNIO_API[PyNIOVariable_SetAttributeString_NUM] =
    (void *)&PyNIOVariable_SetAttributeString;
  PyNIO_API[PyNIOFile_AddHistoryLine_NUM] =
    (void *)&PyNIOFile_AddHistoryLine;
  PyDict_SetItemString(d, "_C_API",
		       PyCObject_FromVoidPtr((void *)PyNIO_API, NULL));

  /* Check for errors */
  if (PyErr_Occurred())
    Py_FatalError("can't initialize module nio");
}
