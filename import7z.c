#include "Python.h"
#include "structmember.h"
#include "osdefs.h"
#include "marshal.h"
#include <time.h>
#include "lzma/7z.h"
#include "lzma/7zCrc.h"
#include "lzma/7zAlloc.h"
#include "lzma/7zFile.h"


#define IS_SOURCE   0x0
#define IS_BYTECODE 0x1
#define IS_PACKAGE  0x2
#define INPUT_BUFSIZE ((size_t)1 << 18)
#if PY_MAJOR_VERSION == 3 && PY_MINOR_VERSION >= 7
#define PYC_HEADER_SIZE 16
#else
#define PYC_HEADER_SIZE 12
#endif

struct st_7z_searchorder {
    char suffix[14];
    int type;
};

#ifdef ALTSEP
_Py_IDENTIFIER(replace);
#endif

/* searchorder_7z defines how we search for a module in the 7z
   archive: we first search for a package __init__, then for
   non-package .pyc, and .py entries. The .pyc entries
   are swapped by initimport7z() if we run in optimized mode. Also,
   '/' is replaced by SEP there. */
static struct st_7z_searchorder searchorder_7z[] = {
    {"/__init__.pyc", IS_PACKAGE | IS_BYTECODE},
    {"/__init__.py", IS_PACKAGE | IS_SOURCE},
    {".pyc", IS_BYTECODE},
    {".py", IS_SOURCE},
    {"", 0}
};

/* importer7z object definition and support */

typedef struct _importer7z Importer7z;

struct _importer7z {
    PyObject_HEAD
    PyObject *archive;  /* pathname of the 7z archive,
                           decoded from the filesystem encoding */
    PyObject *prefix;   /* file prefix: "a/sub/directory/",
                           encoded to the filesystem encoding */
    PyObject *files;    /* dict with file info {path: toc_entry} */
};

static PyObject *Import7zError;
/* read_directory() cache */
static PyObject *directory_cache = NULL;

/* forward decls */
static PyObject *read_directory(PyObject *archive);
static PyObject *get_data(PyObject *archive, PyObject *toc_entry);
static PyObject *get_module_code(Importer7z *self, PyObject *fullname,
                                 int *p_ispackage, PyObject **p_modpath);


#define Importer7z_Check(op) PyObject_TypeCheck(op, &Importer7z_Type)


/* importer7z.__init__
   Split the "subdirectory" from the 7z archive path, lookup a matching
   entry in sys.path_importer_cache, fetch the file directory from there
   if found, or else read it from the archive. */
static int
importer7z_init(Importer7z *self, PyObject *args, PyObject *kwds)
{
    PyObject *path, *files, *tmp;
    PyObject *filename = NULL;
    Py_ssize_t len, flen;

    if (!_PyArg_NoKeywords("importer7z()", kwds))
        return -1;

    if (!PyArg_ParseTuple(args, "O&:importer7z",
                          PyUnicode_FSDecoder, &path))
        return -1;

    if (PyUnicode_READY(path) == -1)
        return -1;

    len = PyUnicode_GET_LENGTH(path);
    if (len == 0) {
        PyErr_SetString(Import7zError, "archive path is empty");
        goto error;
    }

#ifdef ALTSEP
    tmp = _PyObject_CallMethodId(path, &PyId_replace, "CC", ALTSEP, SEP);
    if (!tmp)
        goto error;
    Py_DECREF(path);
    path = tmp;
#endif

    filename = path;
    Py_INCREF(filename);
    flen = len;
    for (;;) {
        struct stat statbuf;
        int rv;

        rv = _Py_stat(filename, &statbuf);
        if (rv == -2)
            goto error;
        if (rv == 0) {
            /* it exists */
            if (!S_ISREG(statbuf.st_mode))
                /* it's a not file */
                Py_CLEAR(filename);
            break;
        }
        Py_CLEAR(filename);
        /* back up one path element */
        flen = PyUnicode_FindChar(path, SEP, 0, flen, -1);
        if (flen == -1)
            break;
        filename = PyUnicode_Substring(path, 0, flen);
        if (filename == NULL)
            goto error;
    }
    if (filename == NULL) {
        PyErr_SetString(Import7zError, "not a 7z file");
        goto error;
    }

    if (PyUnicode_READY(filename) < 0)
        goto error;

    files = PyDict_GetItem(directory_cache, filename);
    if (files == NULL) {
        files = read_directory(filename);
        if (files == NULL)
            goto error;
        if (PyDict_SetItem(directory_cache, filename, files) != 0)
            goto error;
    }
    else
        Py_INCREF(files);
    self->files = files;

    /* Transfer reference */
    self->archive = filename;
    filename = NULL;

    /* Check if there is a prefix directory following the filename. */
    if (flen != len) {
        tmp = PyUnicode_Substring(path, flen+1,
                                  PyUnicode_GET_LENGTH(path));
        if (tmp == NULL)
            goto error;
        self->prefix = tmp;
        if (PyUnicode_READ_CHAR(path, len-1) != SEP) {
            /* add trailing SEP */
            tmp = PyUnicode_FromFormat("%U%c", self->prefix, SEP);
            if (tmp == NULL)
                goto error;
            Py_SETREF(self->prefix, tmp);
        }
    }
    else
        self->prefix = PyUnicode_New(0, 0);
    Py_DECREF(path);
    return 0;

error:
    Py_DECREF(path);
    Py_XDECREF(filename);
    return -1;
}

/* GC support. */
static int
importer7z_traverse(PyObject *obj, visitproc visit, void *arg)
{
    Importer7z *self = (Importer7z *)obj;
    Py_VISIT(self->files);
    return 0;
}

static void
importer7z_dealloc(Importer7z *self)
{
    PyObject_GC_UnTrack(self);
    Py_XDECREF(self->archive);
    Py_XDECREF(self->prefix);
    Py_XDECREF(self->files);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *
importer7z_repr(Importer7z *self)
{
    if (self->archive == NULL)
        return PyUnicode_FromString("<importer7z object \"???\">");
    else if (self->prefix != NULL && PyUnicode_GET_LENGTH(self->prefix) != 0)
        return PyUnicode_FromFormat("<importer7z object \"%U%c%U\">",
                                    self->archive, SEP, self->prefix);
    else
        return PyUnicode_FromFormat("<importer7z object \"%U\">",
                                    self->archive);
}

/* return fullname.split(".")[-1] */
static PyObject *
get_subname(PyObject *fullname)
{
    Py_ssize_t len, dot;
    if (PyUnicode_READY(fullname) < 0)
        return NULL;
    len = PyUnicode_GET_LENGTH(fullname);
    dot = PyUnicode_FindChar(fullname, '.', 0, len, -1);
    if (dot == -1) {
        Py_INCREF(fullname);
        return fullname;
    } else
        return PyUnicode_Substring(fullname, dot+1, len);
}

/* Given a (sub)modulename, write the potential file path in the
   archive (without extension) to the path buffer. Return the
   length of the resulting string.

   return self.prefix + name.replace('.', os.sep) */
static PyObject*
make_filename(PyObject *prefix, PyObject *name)
{
    PyObject *pathobj;
    Py_UCS4 *p, *buf;
    Py_ssize_t len;

    len = PyUnicode_GET_LENGTH(prefix) + PyUnicode_GET_LENGTH(name) + 1;
    p = buf = PyMem_New(Py_UCS4, len);
    if (buf == NULL) {
        PyErr_NoMemory();
        return NULL;
    }

    if (!PyUnicode_AsUCS4(prefix, p, len, 0)) {
        PyMem_Free(buf);
        return NULL;
    }
    p += PyUnicode_GET_LENGTH(prefix);
    len -= PyUnicode_GET_LENGTH(prefix);
    if (!PyUnicode_AsUCS4(name, p, len, 1)) {
        PyMem_Free(buf);
        return NULL;
    }
    for (; *p; p++) {
        if (*p == '.')
            *p = SEP;
    }
    pathobj = PyUnicode_FromKindAndData(PyUnicode_4BYTE_KIND,
                                        buf, p-buf);
    PyMem_Free(buf);
    return pathobj;
}

enum zi_module_info {
    MI_ERROR,
    MI_NOT_FOUND,
    MI_MODULE,
    MI_PACKAGE
};

/* Does this path represent a directory?
   on error, return < 0
   if not a dir, return 0
   if a dir, return 1
*/
static int
check_is_directory(Importer7z *self, PyObject* prefix, PyObject *path)
{
    PyObject *dirpath;
    int res;

    /* See if this is a "directory". If so, it's eligible to be part
       of a namespace package. We test by seeing if the name, with an
       appended path separator, exists. */
    dirpath = PyUnicode_FromFormat("%U%U%c", prefix, path, SEP);
    if (dirpath == NULL)
        return -1;
    /* If dirpath is present in self->files, we have a directory. */
    res = PyDict_Contains(self->files, dirpath);
    Py_DECREF(dirpath);
    return res;
}

/* Return some information about a module. */
static enum zi_module_info
get_module_info(Importer7z *self, PyObject *fullname)
{
    PyObject *subname;
    PyObject *path, *fullpath, *item;
    struct st_7z_searchorder *zso;

    subname = get_subname(fullname);
    if (subname == NULL)
        return MI_ERROR;

    path = make_filename(self->prefix, subname);
    Py_DECREF(subname);
    if (path == NULL)
        return MI_ERROR;

    for (zso = searchorder_7z; *zso->suffix; zso++) {
        fullpath = PyUnicode_FromFormat("%U%s", path, zso->suffix);
        if (fullpath == NULL) {
            Py_DECREF(path);
            return MI_ERROR;
        }
        item = PyDict_GetItem(self->files, fullpath);
        Py_DECREF(fullpath);
        if (item != NULL) {
            Py_DECREF(path);
            if (zso->type & IS_PACKAGE)
                return MI_PACKAGE;
            else
                return MI_MODULE;
        }
    }
    Py_DECREF(path);
    return MI_NOT_FOUND;
}

typedef enum {
    FL_ERROR = -1,       /* error */
    FL_NOT_FOUND,        /* no loader or namespace portions found */
    FL_MODULE_FOUND,     /* module/package found */
    FL_NS_FOUND          /* namespace portion found: */
                         /* *namespace_portion will point to the name */
} find_loader_result;

/* The guts of "find_loader" and "find_module".
*/
static find_loader_result
find_loader(Importer7z *self, PyObject *fullname, PyObject **namespace_portion)
{
    enum zi_module_info mi;

    *namespace_portion = NULL;

    mi = get_module_info(self, fullname);
    if (mi == MI_ERROR)
        return FL_ERROR;
    if (mi == MI_NOT_FOUND) {
        /* Not a module or regular package. See if this is a directory, and
           therefore possibly a portion of a namespace package. */
        find_loader_result result = FL_NOT_FOUND;
        PyObject *subname;
        int is_dir;

        /* We're only interested in the last path component of fullname;
           earlier components are recorded in self->prefix. */
        subname = get_subname(fullname);
        if (subname == NULL) {
            return FL_ERROR;
        }

        is_dir = check_is_directory(self, self->prefix, subname);
        if (is_dir < 0)
            result = FL_ERROR;
        else if (is_dir) {
            /* This is possibly a portion of a namespace
               package. Return the string representing its path,
               without a trailing separator. */
            *namespace_portion = PyUnicode_FromFormat("%U%c%U%U",
                                                      self->archive, SEP,
                                                      self->prefix, subname);
            if (*namespace_portion == NULL)
                result = FL_ERROR;
            else
                result = FL_NS_FOUND;
        }
        Py_DECREF(subname);
        return result;
    }
    /* This is a module or package. */
    return FL_MODULE_FOUND;
}


/* Check whether we can satisfy the import of the module named by
   'fullname'. Return self if we can, None if we can't. */
static PyObject *
importer7z_find_module(PyObject *obj, PyObject *args)
{
    Importer7z *self = (Importer7z *)obj;
    PyObject *path = NULL;
    PyObject *fullname;
    PyObject *namespace_portion = NULL;
    PyObject *result = NULL;

    if (!PyArg_ParseTuple(args, "U|O:importer7z.find_module", &fullname, &path))
        return NULL;

    switch (find_loader(self, fullname, &namespace_portion)) {
    case FL_ERROR:
        return NULL;
    case FL_NS_FOUND:
        /* A namespace portion is not allowed via find_module, so return None. */
        Py_DECREF(namespace_portion);
        /* FALL THROUGH */
    case FL_NOT_FOUND:
        result = Py_None;
        break;
    case FL_MODULE_FOUND:
        result = (PyObject *)self;
        break;
    default:
        PyErr_BadInternalCall();
        return NULL;
    }
    Py_INCREF(result);
    return result;
}


/* Check whether we can satisfy the import of the module named by
   'fullname', or whether it could be a portion of a namespace
   package. Return self if we can load it, a string containing the
   full path if it's a possible namespace portion, None if we
   can't load it. */
static PyObject *
importer7z_find_loader(PyObject *obj, PyObject *args)
{
    Importer7z *self = (Importer7z *)obj;
    PyObject *path = NULL;
    PyObject *fullname;
    PyObject *result = NULL;
    PyObject *namespace_portion = NULL;

    if (!PyArg_ParseTuple(args, "U|O:importer7z.find_module", &fullname, &path))
        return NULL;

    switch (find_loader(self, fullname, &namespace_portion)) {
    case FL_ERROR:
        return NULL;
    case FL_NOT_FOUND:        /* Not found, return (None, []) */
        result = Py_BuildValue("O[]", Py_None);
        break;
    case FL_MODULE_FOUND:     /* Return (self, []) */
        result = Py_BuildValue("O[]", self);
        break;
    case FL_NS_FOUND:         /* Return (None, [namespace_portion]) */
        result = Py_BuildValue("O[O]", Py_None, namespace_portion);
        Py_DECREF(namespace_portion);
        return result;
    default:
        PyErr_BadInternalCall();
        return NULL;
    }
    return result;
}

/* Load and return the module named by 'fullname'. */
static PyObject *
importer7z_load_module(PyObject *obj, PyObject *args)
{
    Importer7z *self = (Importer7z *)obj;
    PyObject *code = NULL, *mod, *dict;
    PyObject *fullname;
    PyObject *modpath = NULL;
    int ispackage;

    if (!PyArg_ParseTuple(args, "U:importer7z.load_module",
                          &fullname))
        return NULL;
    if (PyUnicode_READY(fullname) == -1)
        return NULL;

    code = get_module_code(self, fullname, &ispackage, &modpath);
    if (code == NULL)
        goto error;

    mod = PyImport_AddModuleObject(fullname);
    if (mod == NULL)
        goto error;
    dict = PyModule_GetDict(mod);

    /* mod.__loader__ = self */
    if (PyDict_SetItemString(dict, "__loader__", (PyObject *)self) != 0)
        goto error;

    if (ispackage) {
        /* add __path__ to the module *before* the code gets
           executed */
        PyObject *pkgpath, *fullpath, *subname;
        int err;

        subname = get_subname(fullname);
        if (subname == NULL)
            goto error;

        fullpath = PyUnicode_FromFormat("%U%c%U%U",
                                self->archive, SEP,
                                self->prefix, subname);
        Py_DECREF(subname);
        if (fullpath == NULL)
            goto error;

        pkgpath = Py_BuildValue("[N]", fullpath);
        if (pkgpath == NULL)
            goto error;
        err = PyDict_SetItemString(dict, "__path__", pkgpath);
        Py_DECREF(pkgpath);
        if (err != 0)
            goto error;
    }
    mod = PyImport_ExecCodeModuleObject(fullname, code, modpath, NULL);
    Py_CLEAR(code);
    if (mod == NULL)
        goto error;

    if (Py_VerboseFlag)
        PySys_FormatStderr("import %U # loaded from 7z %U\n",
                           fullname, modpath);
    Py_DECREF(modpath);
    return mod;
error:
    Py_XDECREF(code);
    Py_XDECREF(modpath);
    return NULL;
}

/* Return a string matching __file__ for the named module */
static PyObject *
importer7z_get_filename(PyObject *obj, PyObject *args)
{
    Importer7z *self = (Importer7z *)obj;
    PyObject *fullname, *code, *modpath;
    int ispackage;

    if (!PyArg_ParseTuple(args, "U:importer7z.get_filename",
                          &fullname))
        return NULL;

    /* Deciding the filename requires working out where the code
       would come from if the module was actually loaded */
    code = get_module_code(self, fullname, &ispackage, &modpath);
    if (code == NULL)
        return NULL;
    Py_DECREF(code); /* Only need the path info */

    return modpath;
}

/* Return a bool signifying whether the module is a package or not. */
static PyObject *
importer7z_is_package(PyObject *obj, PyObject *args)
{
    Importer7z *self = (Importer7z *)obj;
    PyObject *fullname;
    enum zi_module_info mi;

    if (!PyArg_ParseTuple(args, "U:importer7z.is_package",
                          &fullname))
        return NULL;

    mi = get_module_info(self, fullname);
    if (mi == MI_ERROR)
        return NULL;
    if (mi == MI_NOT_FOUND) {
        PyErr_Format(Import7zError, "can't find module %R", fullname);
        return NULL;
    }
    return PyBool_FromLong(mi == MI_PACKAGE);
}


static PyObject *
importer7z_get_data(PyObject *obj, PyObject *args)
{
    Importer7z *self = (Importer7z *)obj;
    PyObject *path, *key;
    PyObject *toc_entry;
    Py_ssize_t path_start, path_len, len;

    if (!PyArg_ParseTuple(args, "U:importer7z.get_data", &path))
        return NULL;

#ifdef ALTSEP
    path = _PyObject_CallMethodId((PyObject *)&PyUnicode_Type, &PyId_replace,
                                  "OCC", path, ALTSEP, SEP);
    if (!path)
        return NULL;
#else
    Py_INCREF(path);
#endif
    if (PyUnicode_READY(path) == -1)
        goto error;

    path_len = PyUnicode_GET_LENGTH(path);

    len = PyUnicode_GET_LENGTH(self->archive);
    path_start = 0;
    if (PyUnicode_Tailmatch(path, self->archive, 0, len, -1)
        && PyUnicode_READ_CHAR(path, len) == SEP) {
        path_start = len + 1;
    }

    key = PyUnicode_Substring(path, path_start, path_len);
    if (key == NULL)
        goto error;
    toc_entry = PyDict_GetItem(self->files, key);
    if (toc_entry == NULL) {
        PyErr_SetFromErrnoWithFilenameObject(PyExc_IOError, key);
        Py_DECREF(key);
        goto error;
    }
    Py_DECREF(key);
    Py_DECREF(path);
    return get_data(self->archive, toc_entry);
  error:
    Py_DECREF(path);
    return NULL;
}

static PyObject *
importer7z_get_code(PyObject *obj, PyObject *args)
{
    Importer7z *self = (Importer7z *)obj;
    PyObject *fullname;

    if (!PyArg_ParseTuple(args, "U:importer7z.get_code", &fullname))
        return NULL;

    return get_module_code(self, fullname, NULL, NULL);
}

static PyObject *
importer7z_get_source(PyObject *obj, PyObject *args)
{
    Importer7z *self = (Importer7z *)obj;
    PyObject *toc_entry;
    PyObject *fullname, *subname, *path, *fullpath;
    enum zi_module_info mi;

    if (!PyArg_ParseTuple(args, "U:importer7z.get_source", &fullname))
        return NULL;

    mi = get_module_info(self, fullname);
    if (mi == MI_ERROR)
        return NULL;
    if (mi == MI_NOT_FOUND) {
        PyErr_Format(Import7zError, "can't find module %R", fullname);
        return NULL;
    }

    subname = get_subname(fullname);
    if (subname == NULL)
        return NULL;

    path = make_filename(self->prefix, subname);
    Py_DECREF(subname);
    if (path == NULL)
        return NULL;

    if (mi == MI_PACKAGE)
        fullpath = PyUnicode_FromFormat("%U%c__init__.py", path, SEP);
    else
        fullpath = PyUnicode_FromFormat("%U.py", path);
    Py_DECREF(path);
    if (fullpath == NULL)
        return NULL;

    toc_entry = PyDict_GetItem(self->files, fullpath);
    Py_DECREF(fullpath);
    if (toc_entry != NULL) {
        PyObject *res, *bytes;
        bytes = get_data(self->archive, toc_entry);
        if (bytes == NULL)
            return NULL;
        res = PyUnicode_FromStringAndSize(PyBytes_AS_STRING(bytes),
                                          PyBytes_GET_SIZE(bytes));
        Py_DECREF(bytes);
        return res;
    }

    /* we have the module, but no source */
    Py_INCREF(Py_None);
    return Py_None;
}

PyDoc_STRVAR(doc_find_module,
"find_module(fullname, path=None) -> self or None.\n\
\n\
Search for a module specified by 'fullname'. 'fullname' must be the\n\
fully qualified (dotted) module name. It returns the importer7z\n\
instance itself if the module was found, or None if it wasn't.\n\
The optional 'path' argument is ignored -- it's there for compatibility\n\
with the importer protocol.");

PyDoc_STRVAR(doc_find_loader,
"find_loader(fullname, path=None) -> self, str or None.\n\
\n\
Search for a module specified by 'fullname'. 'fullname' must be the\n\
fully qualified (dotted) module name. It returns the importer7z\n\
instance itself if the module was found, a string containing the\n\
full path name if it's possibly a portion of a namespace package,\n\
or None otherwise. The optional 'path' argument is ignored -- it's\n\
 there for compatibility with the importer protocol.");

PyDoc_STRVAR(doc_load_module,
"load_module(fullname) -> module.\n\
\n\
Load the module specified by 'fullname'. 'fullname' must be the\n\
fully qualified (dotted) module name. It returns the imported\n\
module, or raises Import7zError if it wasn't found.");

PyDoc_STRVAR(doc_get_data,
"get_data(pathname) -> string with file data.\n\
\n\
Return the data associated with 'pathname'. Raise IOError if\n\
the file wasn't found.");

PyDoc_STRVAR(doc_is_package,
"is_package(fullname) -> bool.\n\
\n\
Return True if the module specified by fullname is a package.\n\
Raise Import7zError if the module couldn't be found.");

PyDoc_STRVAR(doc_get_code,
"get_code(fullname) -> code object.\n\
\n\
Return the code object for the specified module. Raise Import7zError\n\
if the module couldn't be found.");

PyDoc_STRVAR(doc_get_source,
"get_source(fullname) -> source string.\n\
\n\
Return the source code for the specified module. Raise Import7zError\n\
if the module couldn't be found, return None if the archive does\n\
contain the module, but has no source for it.");


PyDoc_STRVAR(doc_get_filename,
"get_filename(fullname) -> filename string.\n\
\n\
Return the filename for the specified module.");

static PyMethodDef importer7z_methods[] = {
    {"find_module", importer7z_find_module, METH_VARARGS,
     doc_find_module},
    {"find_loader", importer7z_find_loader, METH_VARARGS,
     doc_find_loader},
    {"load_module", importer7z_load_module, METH_VARARGS,
     doc_load_module},
    {"get_data", importer7z_get_data, METH_VARARGS,
     doc_get_data},
    {"get_code", importer7z_get_code, METH_VARARGS,
     doc_get_code},
    {"get_source", importer7z_get_source, METH_VARARGS,
     doc_get_source},
    {"get_filename", importer7z_get_filename, METH_VARARGS,
     doc_get_filename},
    {"is_package", importer7z_is_package, METH_VARARGS,
     doc_is_package},
    {NULL,              NULL}   /* sentinel */
};

static PyMemberDef importer7z_members[] = {
    {"archive",  T_OBJECT, offsetof(Importer7z, archive),  READONLY},
    {"prefix",   T_OBJECT, offsetof(Importer7z, prefix),   READONLY},
    {"_files",   T_OBJECT, offsetof(Importer7z, files),    READONLY},
    {NULL}
};

PyDoc_STRVAR(importer7z_doc,
"importer7z(archivepath) -> importer7z object\n\
\n\
Create a new importer7z instance. 'archivepath' must be a path to\n\
a zipfile, or to a specific path inside a zipfile. For example, it can be\n\
'/tmp/myimport.zip', or '/tmp/myimport.zip/mydirectory', if mydirectory is a\n\
valid directory inside the archive.\n\
\n\
'Import7zError is raised if 'archivepath' doesn't point to a valid 7z\n\
archive.\n\
\n\
The 'archive' attribute of importer7z objects contains the name of the\n\
zipfile targeted.");

#define DEFERRED_ADDRESS(ADDR) 0

static PyTypeObject Importer7z_Type = {
    PyVarObject_HEAD_INIT(DEFERRED_ADDRESS(&PyType_Type), 0)
    "import7z.importer7z",
    sizeof(Importer7z),
    0,                                          /* tp_itemsize */
    (destructor)importer7z_dealloc,             /* tp_dealloc */
    0,                                          /* tp_print */
    0,                                          /* tp_getattr */
    0,                                          /* tp_setattr */
    0,                                          /* tp_reserved */
    (reprfunc)importer7z_repr,                  /* tp_repr */
    0,                                          /* tp_as_number */
    0,                                          /* tp_as_sequence */
    0,                                          /* tp_as_mapping */
    0,                                          /* tp_hash */
    0,                                          /* tp_call */
    0,                                          /* tp_str */
    PyObject_GenericGetAttr,                    /* tp_getattro */
    0,                                          /* tp_setattro */
    0,                                          /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE |
        Py_TPFLAGS_HAVE_GC,                     /* tp_flags */
    importer7z_doc,                             /* tp_doc */
    importer7z_traverse,                        /* tp_traverse */
    0,                                          /* tp_clear */
    0,                                          /* tp_richcompare */
    0,                                          /* tp_weaklistoffset */
    0,                                          /* tp_iter */
    0,                                          /* tp_iternext */
    importer7z_methods,                         /* tp_methods */
    importer7z_members,                         /* tp_members */
    0,                                          /* tp_getset */
    0,                                          /* tp_base */
    0,                                          /* tp_dict */
    0,                                          /* tp_descr_get */
    0,                                          /* tp_descr_set */
    0,                                          /* tp_dictoffset */
    (initproc)importer7z_init,                  /* tp_init */
    PyType_GenericAlloc,                        /* tp_alloc */
    PyType_GenericNew,                          /* tp_new */
    PyObject_GC_Del,                            /* tp_free */
};


/* implementation */

/* Given a buffer, return the unsigned int that is represented by the first
   4 bytes, encoded as little endian. This partially reimplements
   marshal.c:r_long() */
static unsigned int
get_uint32(const unsigned char *buf)
{
    unsigned int x;
    x =  buf[0];
    x |= (unsigned int)buf[1] <<  8;
    x |= (unsigned int)buf[2] << 16;
    x |= (unsigned int)buf[3] << 24;
    return x;
}

static void
set_file_error(PyObject *archive, int eof)
{
    if (eof) {
        PyErr_SetString(PyExc_EOFError, "EOF read where not expected");
    }
    else {
        PyErr_SetFromErrnoWithFilenameObject(PyExc_OSError, archive);
    }
}

/*
   read_directory(archive) -> files dict (new reference)

   Given a path to a 7z archive, build a dict, mapping file names
   (local to the archive, using SEP as a separator) to toc entries.

   A toc_entry is a tuple:

   (__file__,      # value to use for __file__, available for all files,
                   # encoded to the filesystem encoding
    index,         # index of file
    file_size,     # size of decompressed data
   )
*/
static PyObject *
read_directory(PyObject *archive)
{
    PyObject *files = NULL;
    PyObject *nameobj = NULL;
    PyObject *path = NULL;
    const char *errmsg = NULL;

    ISzAlloc alloc = { SzAlloc, SzFree };
    ISzAlloc alloc_tmp = { SzAllocTemp, SzFreeTemp };

    CFileInStream stream_arc;
    CLookToRead2 stream_look;

    if (InFile_OpenW(&stream_arc.file, PyUnicode_AsUnicode(archive)) != SZ_OK) {
        _PyErr_FormatFromCause(Import7zError,
            "can't open 7z file: %R", archive);
        return NULL;
    }

    FileInStream_CreateVTable(&stream_arc);
    LookToRead2_CreateVTable(&stream_look, False);

    stream_look.buf = (Byte*)ISzAlloc_Alloc(&alloc, INPUT_BUFSIZE);
    stream_look.bufSize = INPUT_BUFSIZE;
    stream_look.realStream = &stream_arc.vt;
    LookToRead2_Init(&stream_look);

    CSzArEx db;
    SzArEx_Init(&db);

    if (SzArEx_Open(&db, &stream_look.vt, &alloc, &alloc_tmp) != SZ_OK) {
        goto file_error;
    }
    files = PyDict_New();
    if (files == NULL) {
        goto error;
    }

    for (uint32_t i = 0; i < db.NumFiles; i++) {
        PyObject *t;
        int err;
        wchar_t name[MAX_PATH];
        size_t name_size = SzArEx_GetFileNameUtf16(&db, i, (UInt16*)name);
        int file_size = (int)SzArEx_GetFileSize(&db, i);

        if (SEP != '/') {
            for (size_t i = 0; i < name_size; i++) {
                if (name[i] == '/') {
                    name[i] = SEP;
                }
            }
        }

        nameobj = PyUnicode_FromUnicode(name, name_size - 1);
        if (nameobj == NULL) {
            goto error;
        }
        if (PyUnicode_READY(nameobj) == -1) {
            goto error;
        }
        path = PyUnicode_FromFormat("%U%c%U", archive, SEP, nameobj);
        if (path == NULL) {
            goto error;
        }
        t = Py_BuildValue("NII", path, i, file_size);
        if (t == NULL) {
            goto error;
        }
        err = PyDict_SetItem(files, nameobj, t);
        Py_CLEAR(nameobj);
        Py_DECREF(t);
        if (err != 0) {
            goto error;
        }
    }
    SzArEx_Free(&db, &alloc);
    ISzAlloc_Free(&alloc, stream_look.buf);
    File_Close(&stream_arc.file);
    return files;

file_error:
    PyErr_Format(Import7zError, "can't read 7z file: %R", archive);
    return NULL;

error:
    SzArEx_Free(&db, &alloc);
    ISzAlloc_Free(&alloc, stream_look.buf);
    File_Close(&stream_arc.file);
    Py_XDECREF(files);
    Py_XDECREF(nameobj);
    return NULL;
}

/* Given a path to a 7z file and a toc_entry, return the (uncompressed)
   data as a new reference. */
static PyObject *
get_data(PyObject *archive, PyObject *toc_entry)
{
    PyObject *data = NULL;
    PyObject *datapath;
    const char *errmsg = NULL;
    unsigned int index, file_size;
    UInt32 idx_blk = 0xFFFFFFFF;
    Byte* out_ptr = 0;
    size_t out_length = 0;
    size_t offset = 0;
    size_t processed = 0;

    ISzAlloc alloc = { SzAlloc, SzFree };
    ISzAlloc alloc_tmp = { SzAllocTemp, SzFreeTemp };
    
    CFileInStream stream_arc;
    CLookToRead2 stream_look;

    if (!PyArg_ParseTuple(toc_entry, "OII", &datapath, &index, &file_size)) {
        return NULL;
    }

    if (InFile_OpenW(&stream_arc.file, PyUnicode_AsUnicode(archive)) != SZ_OK) {
        _PyErr_FormatFromCause(Import7zError, "can't open 7z file: %R", archive);
        return NULL;
    }

    FileInStream_CreateVTable(&stream_arc);
    LookToRead2_CreateVTable(&stream_look, False);

    stream_look.buf = (Byte*)ISzAlloc_Alloc(&alloc, INPUT_BUFSIZE);
    stream_look.bufSize = INPUT_BUFSIZE;
    stream_look.realStream = &stream_arc.vt;
    LookToRead2_Init(&stream_look);

    CSzArEx db;
    SzArEx_Init(&db);
    if (SzArEx_Open(&db, &stream_look.vt, &alloc, &alloc_tmp) != SZ_OK) {
        goto file_error;
    }

    if (SzArEx_Extract(&db, &stream_look.vt, index, &idx_blk, &out_ptr,
                       &out_length, &offset, &processed, &alloc,
                       &alloc_tmp) != SZ_OK) {
        PyErr_SetString(Import7zError, "can't decompress data");
        goto error;
    }
    data = PyBytes_FromStringAndSize(out_ptr + offset, processed);

    IAlloc_Free(&alloc, out_ptr);
    SzArEx_Free(&db, &alloc);
    ISzAlloc_Free(&alloc, stream_look.buf);
    File_Close(&stream_arc.file);

    return data;

file_error:
    PyErr_Format(Import7zError, "can't read 7z file: %R", archive);
    return NULL;

error:
    SzArEx_Free(&db, &alloc);
    ISzAlloc_Free(&alloc, stream_look.buf);
    File_Close(&stream_arc.file);

    return NULL;
}

/* Given the contents of a .pyc file in a buffer, unmarshal the data
   and return the code object. Return None if it the magic word doesn't
   match (we do this instead of raising an exception as we fall back
   to .py if available and we don't want to mask other errors).
   Returns a new reference. */
static PyObject *
unmarshal_code(PyObject *pathname, PyObject *data, time_t mtime)
{
    PyObject *code;
    unsigned char *buf = (unsigned char *)PyBytes_AsString(data);
    Py_ssize_t size = PyBytes_Size(data);

    if (size < PYC_HEADER_SIZE) {
        PyErr_SetString(Import7zError,
                        "bad pyc data");
        return NULL;
    }

    if (get_uint32(buf) != (unsigned int)PyImport_GetMagicNumber()) {
        if (Py_VerboseFlag) {
            PySys_FormatStderr("# %R has bad magic\n",
                               pathname);
        }
        Py_INCREF(Py_None);
        return Py_None;  /* signal caller to try alternative */
    }

    /* XXX the pyc's size field is ignored; timestamp collisions are probably
       unimportant with zip files. */
    code = PyMarshal_ReadObjectFromString((char *)buf + PYC_HEADER_SIZE, size - PYC_HEADER_SIZE);
    if (code == NULL) {
        return NULL;
    }
    if (!PyCode_Check(code)) {
        Py_DECREF(code);
        PyErr_Format(PyExc_TypeError,
             "compiled module %R is not a code object",
             pathname);
        return NULL;
    }
    return code;
}

/* Replace any occurrences of "\r\n?" in the input string with "\n".
   This converts DOS and Mac line endings to Unix line endings.
   Also append a trailing "\n" to be compatible with
   PyParser_SimpleParseFile(). Returns a new reference. */
static PyObject *
normalize_line_endings(PyObject *source)
{
    char *buf, *q, *p;
    PyObject *fixed_source;
    int len = 0;

    p = PyBytes_AsString(source);
    if (p == NULL) {
        return PyBytes_FromStringAndSize("\n\0", 2);
    }

    /* one char extra for trailing \n and one for terminating \0 */
    buf = (char *)PyMem_Malloc(PyBytes_Size(source) + 2);
    if (buf == NULL) {
        PyErr_SetString(PyExc_MemoryError,
                        "import7z: no memory to allocate "
                        "source buffer");
        return NULL;
    }
    /* replace "\r\n?" by "\n" */
    for (q = buf; *p != '\0'; p++) {
        if (*p == '\r') {
            *q++ = '\n';
            if (*(p + 1) == '\n')
                p++;
        }
        else
            *q++ = *p;
        len++;
    }
    *q++ = '\n';  /* add trailing \n */
    *q = '\0';
    fixed_source = PyBytes_FromStringAndSize(buf, len + 2);
    PyMem_Free(buf);
    return fixed_source;
}

/* Given a string buffer containing Python source code, compile it
   and return a code object as a new reference. */
static PyObject *
compile_source(PyObject *pathname, PyObject *source)
{
    PyObject *code, *fixed_source;

    fixed_source = normalize_line_endings(source);
    if (fixed_source == NULL) {
        return NULL;
    }

    code = Py_CompileStringObject(PyBytes_AsString(fixed_source),
                                  pathname, Py_file_input, NULL, -1);

    Py_DECREF(fixed_source);
    return code;
}

/* Return the code object for the module named by 'fullname' from the
   7z archive as a new reference. */
static PyObject *
get_code_from_data(Importer7z *self, int ispackage, int isbytecode,
                   time_t mtime, PyObject *toc_entry)
{
    PyObject *data, *modpath, *code;

    data = get_data(self->archive, toc_entry);
    if (data == NULL)
        return NULL;

    modpath = PyTuple_GetItem(toc_entry, 0);
    if (isbytecode)
        code = unmarshal_code(modpath, data, mtime);
    else
        code = compile_source(modpath, data);
    Py_DECREF(data);
    return code;
}

/* Get the code object associated with the module specified by
   'fullname'. */
static PyObject *
get_module_code(Importer7z *self, PyObject *fullname,
                int *p_ispackage, PyObject **p_modpath)
{
    PyObject *code = NULL, *toc_entry, *subname;
    PyObject *path, *fullpath = NULL;
    struct st_7z_searchorder *zso;

    subname = get_subname(fullname);
    if (subname == NULL)
        return NULL;

    path = make_filename(self->prefix, subname);
    Py_DECREF(subname);
    if (path == NULL)
        return NULL;

    for (zso = searchorder_7z; *zso->suffix; zso++) {
        code = NULL;

        fullpath = PyUnicode_FromFormat("%U%s", path, zso->suffix);
        if (fullpath == NULL)
            goto exit;

        if (Py_VerboseFlag > 1)
            PySys_FormatStderr("# trying %U%c%U\n",
                               self->archive, (int)SEP, fullpath);
        toc_entry = PyDict_GetItem(self->files, fullpath);
        if (toc_entry != NULL) {
            time_t mtime = 0;
            int ispackage = zso->type & IS_PACKAGE;
            int isbytecode = zso->type & IS_BYTECODE;
            mtime = (time_t)-1;

            Py_CLEAR(fullpath);
            if (p_ispackage != NULL)
                *p_ispackage = ispackage;
            code = get_code_from_data(self, ispackage,
                                      isbytecode, mtime,
                                      toc_entry);
            if (code == Py_None) {
                /* bad magic number or non-matching mtime
                   in byte code, try next */
                Py_DECREF(code);
                continue;
            }
            if (code != NULL && p_modpath != NULL) {
                *p_modpath = PyTuple_GetItem(toc_entry, 0);
                Py_INCREF(*p_modpath);
            }
            goto exit;
        }
        else
            Py_CLEAR(fullpath);
    }
    PyErr_Format(Import7zError, "can't find module %R", fullname);
exit:
    Py_DECREF(path);
    Py_XDECREF(fullpath);
    return code;
}


/* Module init */

PyDoc_STRVAR(import7z_doc,
"import7z provides support for importing Python modules from 7z archives.\n\
\n\
This module exports three objects:\n\
- importer7z: a class; its constructor takes a path to a 7z archive.\n\
- Import7zError: exception raised by importer7z objects. It's a\n\
  subclass of ImportError, so it can be caught as ImportError, too.\n\
- _directory_cache: a dict, mapping archive paths to zip directory\n\
  info dicts, as used in importer7z._files.\n\
\n\
It is usually not needed to use the import7z module explicitly; it is\n\
used by the builtin import mechanism for sys.path items that are paths\n\
to 7z archives.");

static struct PyModuleDef import7zmodule = {
    PyModuleDef_HEAD_INIT,
    "import7z",
    import7z_doc,
    -1,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL
};

PyMODINIT_FUNC
PyInit_import7z(void)
{
    PyObject *mod;

    if (PyType_Ready(&Importer7z_Type) < 0)
        return NULL;

    /* Correct directory separator */
    searchorder_7z[0].suffix[0] = SEP;
    searchorder_7z[1].suffix[0] = SEP;

    CrcGenerateTable();

    mod = PyModule_Create(&import7zmodule);
    if (mod == NULL)
        return NULL;

    Import7zError = PyErr_NewException("import7z.Import7zError",
                                        PyExc_ImportError, NULL);
    if (Import7zError == NULL)
        return NULL;

    Py_INCREF(Import7zError);
    if (PyModule_AddObject(mod, "Import7zError",
                           Import7zError) < 0)
        return NULL;

    Py_INCREF(&Importer7z_Type);
    if (PyModule_AddObject(mod, "importer7z",
                           (PyObject *)&Importer7z_Type) < 0)
        return NULL;

    directory_cache = PyDict_New();
    if (directory_cache == NULL)
        return NULL;
    Py_INCREF(directory_cache);
    if (PyModule_AddObject(mod, "_directory_cache",
                           directory_cache) < 0)
        return NULL;
    return mod;
}
