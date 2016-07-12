/* Big parts of this file are copied (with modifications) from
   CPython/Objects/tupleobject.c.

   Portions Copyright (c) PSF (and other CPython copyright holders).
   Portions Copyright (c) 2016-present MagicStack Inc.
   License: PSFL v2; see CPython/LICENSE for details.
*/

#include "recordobj.h"


static PyObject * record_iter(PyObject *);
static PyObject * record_new_items_iter(PyObject *);

static ApgRecordObject *free_list[ApgRecord_MAXSAVESIZE];
static int numfree[ApgRecord_MAXSAVESIZE];


PyObject *
ApgRecord_New(PyObject *mapping, Py_ssize_t size)
{
    ApgRecordObject *o;
    Py_ssize_t i;

    if (size < 1 || mapping == NULL) {
        PyErr_BadInternalCall();
        return NULL;
    }

    if (size < ApgRecord_MAXSAVESIZE && (o = free_list[size]) != NULL) {
        free_list[size] = (ApgRecordObject *) o->ob_item[0];
        numfree[size]--;
        _Py_NewReference((PyObject *)o);
    }
    else {
        /* Check for overflow */
        if ((size_t)size > ((size_t)PY_SSIZE_T_MAX - sizeof(ApgRecordObject) -
                    sizeof(PyObject *)) / sizeof(PyObject *)) {
            return PyErr_NoMemory();
        }
        o = PyObject_GC_NewVar(ApgRecordObject, &ApgRecord_Type, size);
        if (o == NULL) {
            return NULL;
        }
    }

    for (i = 0; i < size; i++) {
        o->ob_item[i] = NULL;
    }

    Py_INCREF(mapping);
    o->mapping = mapping;
    o->mapping_hash = -1;

    o->self_hash = -1;

    _PyObject_GC_TRACK(o);
    return (PyObject *) o;
}


static void
record_dealloc(ApgRecordObject *o)
{
    Py_ssize_t i;
    Py_ssize_t len = Py_SIZE(o);

    PyObject_GC_UnTrack(o);

    o->self_hash = -1;

    Py_XDECREF(o->mapping);
    o->mapping = NULL;
    o->mapping_hash = -1;

    Py_TRASHCAN_SAFE_BEGIN(o)
    if (len > 0) {
        i = len;
        while (--i >= 0) {
            Py_XDECREF(o->ob_item[i]);
            o->ob_item[i] = NULL;
        }

        if (len < ApgRecord_MAXSAVESIZE &&
            numfree[len] < ApgRecord_MAXFREELIST &&
            ApgRecord_CheckExact(o))
        {
            o->ob_item[0] = (PyObject *) free_list[len];
            numfree[len]++;
            free_list[len] = o;
            goto done; /* return */
        }
    }
    Py_TYPE(o)->tp_free((PyObject *)o);
done:
    Py_TRASHCAN_SAFE_END(o)
}


static int
record_traverse(ApgRecordObject *o, visitproc visit, void *arg)
{
    Py_ssize_t i;

    Py_VISIT(o->mapping);

    for (i = Py_SIZE(o); --i >= 0;) {
        if (o->ob_item[i] != NULL) {
            Py_VISIT(o->ob_item[i]);
        }
    }

    return 0;
}


static Py_ssize_t
record_length(ApgRecordObject *o)
{
    return Py_SIZE(o);
}


static Py_hash_t
record_get_mapping_hash(ApgRecordObject *v)
{
    PyObject * repr;

    if (v->mapping_hash != -1) {
        return v->mapping_hash;
    }

    repr = PyObject_Repr(v->mapping);
    if (repr == NULL) {
        return -1;
    }

    v->mapping_hash = PyObject_Hash(repr);
    Py_DECREF(repr);

    return v->mapping_hash;
}


static Py_hash_t
record_hash(ApgRecordObject *v)
{
    Py_uhash_t x;  /* Unsigned for defined overflow behavior. */
    Py_hash_t y;
    Py_ssize_t len;
    PyObject **p;
    Py_uhash_t mult;

    if (v->self_hash != -1) {
        return v->self_hash;
    }

    len = Py_SIZE(v);
    mult = _PyHASH_MULTIPLIER;

    x = 0x345678UL;
    y = record_get_mapping_hash(v);
    if (y == -1) {
        return -1;
    }
    x = (x ^ y) * mult;
    mult += (Py_hash_t)(82520UL + len + len + 2);

    p = v->ob_item;
    while (--len >= 0) {
        y = PyObject_Hash(*p++);
        if (y == -1) {
            return -1;
        }
        x = (x ^ y) * mult;
        /* the cast might truncate len; that doesn't change hash stability */
        mult += (Py_hash_t)(82520UL + len + len + 2);
    }
    x += 97531UL;
    if (x == (Py_uhash_t)-1) {
        x = -2;
    }
    v->self_hash = x;
    return x;
}


static PyObject *
record_richcompare(PyObject *v, PyObject *w, int op)
{
    ApgRecordObject *vt, *wt;
    Py_ssize_t i;
    Py_ssize_t vlen, wlen;
    int comp;

    if (!ApgRecord_CheckExact(v) || !ApgRecord_CheckExact(w)) {
        Py_RETURN_NOTIMPLEMENTED;
    }

    vt = (ApgRecordObject *)v;
    wt = (ApgRecordObject *)w;

    vlen = Py_SIZE(vt);
    wlen = Py_SIZE(wt);

    if (op == Py_EQ && vlen != wlen) {
        /* Checking if v == w, but len(v) != len(w): return False */
        Py_RETURN_FALSE;
    }

    if (op == Py_NE && vlen != wlen) {
        /* Checking if v != w, and len(v) != len(w): return True */
        Py_RETURN_TRUE;
    }

    if (vt->mapping != wt->mapping) {
        /* v and w don't have the same mapping */
        comp = PyObject_RichCompareBool(vt->mapping, wt->mapping, Py_EQ);
        if (comp < 0) {
            return NULL;
        }
        if (!comp) {
            /* Mapping of v is different from mapping of w */
            if (op == Py_NE) {
                /* If we're checking if v != w: return True */
                Py_RETURN_TRUE;
            }
            else if (op == Py_EQ) {
                /* If we're checking if v == w: return False */
                Py_RETURN_FALSE;
            }
        }
    }

    /* Search for the first index where items are different.
     * Note that because tuples are immutable, it's safe to reuse
     * vlen and wlen across the comparison calls.
     */
    for (i = 0; i < vlen && i < wlen; i++) {
        comp = PyObject_RichCompareBool(vt->ob_item[i],
                                        wt->ob_item[i], Py_EQ);
        if (comp < 0) {
            return NULL;
        }
        if (!comp) {
            break;
        }
    }

    if (i >= vlen || i >= wlen) {
        /* No more items to compare -- compare sizes */
        int cmp;
        switch (op) {
            case Py_LT: cmp = vlen <  wlen; break;
            case Py_LE: cmp = vlen <= wlen; break;
            case Py_EQ: cmp = vlen == wlen; break;
            case Py_NE: cmp = vlen != wlen; break;
            case Py_GT: cmp = vlen >  wlen; break;
            case Py_GE: cmp = vlen >= wlen; break;
            default: return NULL; /* cannot happen */
        }
        if (cmp) {
            Py_RETURN_TRUE;
        }
        else {
            Py_RETURN_FALSE;
        }
    }

    /* We have an item that differs -- shortcuts for EQ/NE */
    if (op == Py_EQ) {
        Py_RETURN_FALSE;
    }
    if (op == Py_NE) {
        Py_RETURN_TRUE;
    }

    /* Compare the final item again using the proper operator */
    return PyObject_RichCompare(vt->ob_item[i], wt->ob_item[i], op);
}


static PyObject *
record_item(ApgRecordObject *o, Py_ssize_t i)
{
    if (i < 0 || i >= Py_SIZE(o)) {
        PyErr_SetString(PyExc_IndexError, "record index out of range");
        return NULL;
    }
    Py_INCREF(o->ob_item[i]);
    return o->ob_item[i];
}


static PyObject *
record_subscript(ApgRecordObject* o, PyObject* item)
{
    if (PyIndex_Check(item)) {
        Py_ssize_t i = PyNumber_AsSsize_t(item, PyExc_IndexError);
        if (i == -1 && PyErr_Occurred())
            return NULL;
        if (i < 0) {
            i += Py_SIZE(o);
        }
        return record_item(o, i);
    }
    else {
        PyObject *mapped;
        mapped = PyObject_GetItem(o->mapping, item);
        if (mapped != NULL) {
            Py_ssize_t i;
            PyObject *result;

            if (!PyIndex_Check(mapped)) {
                Py_DECREF(mapped);
                goto noitem;
            }

            i = PyNumber_AsSsize_t(mapped, PyExc_IndexError);
            Py_DECREF(mapped);

            if (i < 0 || PyErr_Occurred()) {
                goto noitem;
            }

            result = record_item(o, i);
            if (result == NULL) {
                PyErr_Clear();
                goto noitem;
            }
            return result;
        }
        else {
            goto noitem;
        }
    }

noitem:
    _PyErr_SetKeyError(item);
    return NULL;
}


static PyObject *
record_repr(ApgRecordObject *v)
{
    Py_ssize_t i, n;
    PyObject *keys_iter;
    _PyUnicodeWriter writer;

    n = Py_SIZE(v);
    assert(n > 0);

    keys_iter = PyObject_GetIter(v->mapping);
    if (keys_iter == NULL) {
        return NULL;
    }

    i = Py_ReprEnter((PyObject *)v);
    if (i != 0) {
        return i > 0 ? PyUnicode_FromString("<Record ...>") : NULL;
    }

    _PyUnicodeWriter_Init(&writer);
    writer.overallocate = 1;
    writer.min_length = 12; /* <Record a=1> */

    if (_PyUnicodeWriter_WriteASCIIString(&writer, "<Record ", 8) < 0) {
        goto error;
    }

    for (i = 0; i < n; ++i) {
        PyObject *key;
        PyObject *key_repr;
        PyObject *val_repr;

        if (i > 0) {
            if (_PyUnicodeWriter_WriteChar(&writer, ' ') < 0) {
                goto error;
            }
        }

        if (Py_EnterRecursiveCall(" while getting the repr of a record")) {
            goto error;
        }
        val_repr = PyObject_Repr(v->ob_item[i]);
        Py_LeaveRecursiveCall();
        if (val_repr == NULL) {
            goto error;
        }

        key = PyIter_Next(keys_iter);
        if (key == NULL) {
            Py_DECREF(val_repr);
            PyErr_SetString(PyExc_RuntimeError, "invalid record mapping");
            goto error;
        }

        key_repr = PyObject_Str(key);
        Py_DECREF(key);
        if (key_repr == NULL) {
            Py_DECREF(val_repr);
            goto error;
        }

        if (_PyUnicodeWriter_WriteStr(&writer, key_repr) < 0) {
            Py_DECREF(key_repr);
            Py_DECREF(val_repr);
            goto error;
        }
        Py_DECREF(key_repr);

        if (_PyUnicodeWriter_WriteChar(&writer, '=') < 0) {
            Py_DECREF(val_repr);
            goto error;
        }

        if (_PyUnicodeWriter_WriteStr(&writer, val_repr) < 0) {
            Py_DECREF(val_repr);
            goto error;
        }
        Py_DECREF(val_repr);
    }

    if (_PyUnicodeWriter_WriteChar(&writer, '>') < 0) {
        goto error;
    }

    Py_DECREF(keys_iter);
    Py_ReprLeave((PyObject *)v);
    return _PyUnicodeWriter_Finish(&writer);

error:
    Py_DECREF(keys_iter);
    _PyUnicodeWriter_Dealloc(&writer);
    Py_ReprLeave((PyObject *)v);
    return NULL;
}



static PyObject *
record_values(PyObject *o, PyObject *args)
{
    return record_iter(o);
}


static PyObject *
record_keys(PyObject *o, PyObject *args)
{
    if (!ApgRecord_CheckExact(o)) {
        PyErr_BadInternalCall();
        return NULL;
    }

    return PyObject_GetIter(((ApgRecordObject*)o)->mapping);
}


static PyObject *
record_items(PyObject *o, PyObject *args)
{
    if (!ApgRecord_CheckExact(o)) {
        PyErr_BadInternalCall();
        return NULL;
    }

    return record_new_items_iter(o);
}


static PySequenceMethods record_as_sequence = {
    (lenfunc)record_length,                          /* sq_length */
    0,                                               /* sq_concat */
    0,                                               /* sq_repeat */
    (ssizeargfunc)record_item,                       /* sq_item */
    0,                                               /* sq_slice */
    0,                                               /* sq_ass_item */
    0,                                               /* sq_ass_slice */
    0,                                               /* sq_contains */
};


static PyMappingMethods record_as_mapping = {
    (lenfunc)record_length,                          /* mp_length */
    (binaryfunc)record_subscript,                    /* mp_subscript */
    0                                                /* mp_ass_subscript */
};


static PyMethodDef record_methods[] = {
    {"values",          (PyCFunction)record_values, METH_NOARGS},
    {"keys",            (PyCFunction)record_keys, METH_NOARGS},
    {"items",           (PyCFunction)record_items, METH_NOARGS},
    {NULL,              NULL}           /* sentinel */
};


PyTypeObject ApgRecord_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    "Record",                                        /* tp_name */
    sizeof(ApgRecordObject) - sizeof(PyObject *),    /* tp_basic_size */
    sizeof(PyObject *),                              /* tp_itemsize */
    (destructor)record_dealloc,                      /* tp_dealloc */
    0,                                               /* tp_print */
    0,                                               /* tp_getattr */
    0,                                               /* tp_setattr */
    0,                                               /* tp_reserved */
    (reprfunc)record_repr,                           /* tp_repr */
    0,                                               /* tp_as_number */
    &record_as_sequence,                             /* tp_as_sequence */
    &record_as_mapping,                              /* tp_as_mapping */
    (hashfunc)record_hash,                           /* tp_hash */
    0,                                               /* tp_call */
    0,                                               /* tp_str */
    PyObject_GenericGetAttr,                         /* tp_getattro */
    0,                                               /* tp_setattro */
    0,                                               /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC |
        Py_TPFLAGS_BASETYPE,                         /* tp_flags */
    0,                                               /* tp_doc */
    (traverseproc)record_traverse,                   /* tp_traverse */
    0,                                               /* tp_clear */
    record_richcompare,                              /* tp_richcompare */
    0,                                               /* tp_weaklistoffset */
    record_iter,                                     /* tp_iter */
    0,                                               /* tp_iternext */
    record_methods,                                  /* tp_methods */
    0,                                               /* tp_members */
    0,                                               /* tp_getset */
    0,                                               /* tp_base */
    0,                                               /* tp_dict */
    0,                                               /* tp_descr_get */
    0,                                               /* tp_descr_set */
    0,                                               /* tp_dictoffset */
    0,                                               /* tp_init */
    0,                                               /* tp_alloc */
    0,                                               /* tp_new */
    PyObject_GC_Del,                                 /* tp_free */
};


/* Record Iterator */


typedef struct {
    PyObject_HEAD
    Py_ssize_t it_index;
    ApgRecordObject *it_seq; /* Set to NULL when iterator is exhausted */
} ApgRecordIterObject;


static void
record_iter_dealloc(ApgRecordIterObject *it)
{
    _PyObject_GC_UNTRACK(it);
    Py_XDECREF(it->it_seq);
    PyObject_GC_Del(it);
}


static int
record_iter_traverse(ApgRecordIterObject *it, visitproc visit, void *arg)
{
    Py_VISIT(it->it_seq);
    return 0;
}


static PyObject *
record_iter_next(ApgRecordIterObject *it)
{
    ApgRecordObject *seq;
    PyObject *item;

    assert(it != NULL);
    seq = it->it_seq;
    if (seq == NULL)
        return NULL;
    assert(ApgRecord_CheckExact(seq));

    if (it->it_index < Py_SIZE(seq)) {
        item = ApgRecord_GET_ITEM(seq, it->it_index);
        ++it->it_index;
        Py_INCREF(item);
        return item;
    }

    it->it_seq = NULL;
    Py_DECREF(seq);
    return NULL;
}


static PyObject *
record_iter_len(ApgRecordIterObject *it)
{
    Py_ssize_t len = 0;
    if (it->it_seq) {
        len = Py_SIZE(it->it_seq) - it->it_index;
    }
    return PyLong_FromSsize_t(len);
}


PyDoc_STRVAR(record_iter_len_doc,
             "Private method returning an estimate of len(list(it)).");


static PyMethodDef record_iter_methods[] = {
    {"__length_hint__", (PyCFunction)record_iter_len, METH_NOARGS,
        record_iter_len_doc},
    {NULL,              NULL}           /* sentinel */
};


PyTypeObject ApgRecordIter_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    "RecordIterator",                           /* tp_name */
    sizeof(ApgRecordIterObject),                /* tp_basicsize */
    0,                                          /* tp_itemsize */
    /* methods */
    (destructor)record_iter_dealloc,            /* tp_dealloc */
    0,                                          /* tp_print */
    0,                                          /* tp_getattr */
    0,                                          /* tp_setattr */
    0,                                          /* tp_reserved */
    0,                                          /* tp_repr */
    0,                                          /* tp_as_number */
    0,                                          /* tp_as_sequence */
    0,                                          /* tp_as_mapping */
    0,                                          /* tp_hash */
    0,                                          /* tp_call */
    0,                                          /* tp_str */
    PyObject_GenericGetAttr,                    /* tp_getattro */
    0,                                          /* tp_setattro */
    0,                                          /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,    /* tp_flags */
    0,                                          /* tp_doc */
    (traverseproc)record_iter_traverse,         /* tp_traverse */
    0,                                          /* tp_clear */
    0,                                          /* tp_richcompare */
    0,                                          /* tp_weaklistoffset */
    PyObject_SelfIter,                          /* tp_iter */
    (iternextfunc)record_iter_next,             /* tp_iternext */
    record_iter_methods,                        /* tp_methods */
    0,
};


static PyObject *
record_iter(PyObject *seq)
{
    ApgRecordIterObject *it;

    if (!ApgRecord_CheckExact(seq)) {
        PyErr_BadInternalCall();
        return NULL;
    }
    it = PyObject_GC_New(ApgRecordIterObject, &ApgRecordIter_Type);
    if (it == NULL)
        return NULL;
    it->it_index = 0;
    Py_INCREF(seq);
    it->it_seq = (ApgRecordObject *)seq;
    _PyObject_GC_TRACK(it);
    return (PyObject *)it;
}


/* Record Items Iterator */


typedef struct {
    PyObject_HEAD
    Py_ssize_t it_index;
    PyObject *it_map_iter;
    ApgRecordObject *it_seq; /* Set to NULL when iterator is exhausted */
} ApgRecordItemsObject;


static void
record_items_dealloc(ApgRecordItemsObject *it)
{
    _PyObject_GC_UNTRACK(it);
    Py_XDECREF(it->it_map_iter);
    Py_XDECREF(it->it_seq);
    PyObject_GC_Del(it);
}


static int
record_items_traverse(ApgRecordItemsObject *it, visitproc visit, void *arg)
{
    Py_VISIT(it->it_map_iter);
    Py_VISIT(it->it_seq);
    return 0;
}


static PyObject *
record_items_next(ApgRecordItemsObject *it)
{
    ApgRecordObject *seq;
    PyObject *key;
    PyObject *val;
    PyObject *tup;

    assert(it != NULL);
    seq = it->it_seq;
    if (seq == NULL) {
        return NULL;
    }
    assert(ApgRecord_CheckExact(seq));
    assert(it->it_map_iter != NULL);

    key = PyIter_Next(it->it_map_iter);
    if (key == NULL) {
        /* likely it_map_iter had less items than seq has values */
        goto exhausted;
    }

    if (it->it_index < Py_SIZE(seq)) {
        val = ApgRecord_GET_ITEM(seq, it->it_index);
        ++it->it_index;
        Py_INCREF(val);
    }
    else {
        /* it_map_iter had more items than seq has values */
        Py_CLEAR(key);
        goto exhausted;
    }

    tup = PyTuple_New(2);
    if (tup == NULL) {
        Py_CLEAR(val);
        Py_CLEAR(key);
        goto exhausted;
    }

    PyTuple_SET_ITEM(tup, 0, key);
    PyTuple_SET_ITEM(tup, 1, val);
    return tup;

exhausted:
    Py_CLEAR(it->it_map_iter);
    Py_CLEAR(it->it_seq);
    return NULL;
}


static PyObject *
record_items_len(ApgRecordItemsObject *it)
{
    Py_ssize_t len = 0;
    if (it->it_seq) {
        len = Py_SIZE(it->it_seq) - it->it_index;
    }
    return PyLong_FromSsize_t(len);
}


PyDoc_STRVAR(record_items_len_doc,
             "Private method returning an estimate of len(list(it())).");


static PyMethodDef record_items_methods[] = {
    {"__length_hint__", (PyCFunction)record_items_len, METH_NOARGS,
        record_items_len_doc},
    {NULL,              NULL}           /* sentinel */
};


PyTypeObject ApgRecordItems_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    "RecordItemsIterator",                      /* tp_name */
    sizeof(ApgRecordItemsObject),               /* tp_basicsize */
    0,                                          /* tp_itemsize */
    /* methods */
    (destructor)record_items_dealloc,           /* tp_dealloc */
    0,                                          /* tp_print */
    0,                                          /* tp_getattr */
    0,                                          /* tp_setattr */
    0,                                          /* tp_reserved */
    0,                                          /* tp_repr */
    0,                                          /* tp_as_number */
    0,                                          /* tp_as_sequence */
    0,                                          /* tp_as_mapping */
    0,                                          /* tp_hash */
    0,                                          /* tp_call */
    0,                                          /* tp_str */
    PyObject_GenericGetAttr,                    /* tp_getattro */
    0,                                          /* tp_setattro */
    0,                                          /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,    /* tp_flags */
    0,                                          /* tp_doc */
    (traverseproc)record_items_traverse,        /* tp_traverse */
    0,                                          /* tp_clear */
    0,                                          /* tp_richcompare */
    0,                                          /* tp_weaklistoffset */
    PyObject_SelfIter,                          /* tp_iter */
    (iternextfunc)record_items_next,            /* tp_iternext */
    record_items_methods,                       /* tp_methods */
    0,
};


static PyObject *
record_new_items_iter(PyObject *seq)
{
    ApgRecordItemsObject *it;
    PyObject *map_iter;

    if (!ApgRecord_CheckExact(seq)) {
        PyErr_BadInternalCall();
        return NULL;
    }

    map_iter = PyObject_GetIter(((ApgRecordObject*)seq)->mapping);
    if (map_iter == NULL) {
        return NULL;
    }

    it = PyObject_GC_New(ApgRecordItemsObject, &ApgRecordItems_Type);
    if (it == NULL)
        return NULL;

    it->it_map_iter = map_iter;
    it->it_index = 0;
    Py_INCREF(seq);
    it->it_seq = (ApgRecordObject *)seq;
    _PyObject_GC_TRACK(it);

    return (PyObject *)it;
}