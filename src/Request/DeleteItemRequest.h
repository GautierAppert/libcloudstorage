/*****************************************************************************
 * DeleteItemRequest.h : DeleteItemRequest headers
 *
 *****************************************************************************
 * Copyright (C) 2016-2016 VideoLAN
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

#ifndef DELETEITEMREQUEST_H
#define DELETEITEMREQUEST_H

#include "Request.h"

namespace cloudstorage {

class DeleteItemRequest : public Request<EitherError<void>> {
 public:
  DeleteItemRequest(std::shared_ptr<CloudProvider>, IItem::Pointer item,
                    DeleteItemCallback);
  ~DeleteItemRequest();

 private:
  IItem::Pointer item_;
  DeleteItemCallback callback_;
};

}  // namespace cloudstorage

#endif  // DELETEITEMREQUEST_H
