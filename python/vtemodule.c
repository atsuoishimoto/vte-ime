/*
 * Copyright (C) 2002 Red Hat, Inc.
 *
 * This is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Library General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#ident "$Id: vtemodule.c,v 1.3 2004/04/20 05:22:54 nalin Exp $"

#ifdef HAVE_CONFIG_H
#include "../config.h"
#endif
#include <Python.h>
#include <pygobject.h>
#include <pygtk/pygtk.h>
#include "../src/vte.h"

extern void pyvte_register_classes(PyObject * d);
extern PyMethodDef pyvte_functions[];
extern DL_EXPORT(void) initvte(void);
extern PyTypeObject PyVteTerminal_Type;

DL_EXPORT(void)
init_vte(void)
{
	PyObject *m, *d;

	init_pygobject();
	init_pygtk();

	m = Py_InitModule("_vte", pyvte_functions);
	d = PyModule_GetDict(m);

	pyvte_register_classes(d);

	if (PyErr_Occurred()) {
		Py_FatalError("can't initialise module vte");
	}
}
