/*
 * This file is part of the FirelandsCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Affero General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Affero General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef PathMovementBase_h__
#define PathMovementBase_h__

#include "Define.h"

template<class Entity, class BasePath>
class PathMovementBase
{
public:
    PathMovementBase() : _path(), _currentNode(0) { }
    virtual ~PathMovementBase() { };

    uint32 GetCurrentNode() const { return _currentNode; }

protected:
    BasePath _path;
    uint32 _currentNode;
};

#endif // PathMovementBase_h__
