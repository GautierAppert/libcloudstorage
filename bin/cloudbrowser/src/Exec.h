/*****************************************************************************
 * Exec.h
 *
 *****************************************************************************
 * Copyright (C) 2016 VideoLAN
 *
 * Authors: Paweł Wegner <pawel.wegner95@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/
#ifndef EXEC_H
#define EXEC_H

#ifdef _MSC_VER

#ifdef CLOUDBROWSER_LIBRARY
#define CLOUDBROWSER_API __declspec(dllexport)
#else
#define CLOUDBROWSER_API __declspec(dllimport)
#endif

#else
#define CLOUDBROWSER_API
#endif  // _MSC_VER

CLOUDBROWSER_API int exec_cloudbrowser(int argc, char** argv);

#endif  // EXEC_H
