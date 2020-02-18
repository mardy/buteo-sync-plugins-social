/****************************************************************************
 **
 ** Copyright (c) 2020 Open Mobile Platform LLC
 **
 ** This program/library is free software; you can redistribute it and/or
 ** modify it under the terms of the GNU Lesser General Public License
 ** version 2.1 as published by the Free Software Foundation.
 **
 ** This program/library is distributed in the hope that it will be useful,
 ** but WITHOUT ANY WARRANTY; without even the implied warranty of
 ** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 ** Lesser General Public License for more details.
 **
 ** You should have received a copy of the GNU Lesser General Public
 ** License along with this program/library; if not, write to the Free
 ** Software Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 ** 02110-1301 USA
 **
 ****************************************************************************/

#include "dropboxbackupquerysyncadaptor.h"

DropboxBackupQuerySyncAdaptor::DropboxBackupQuerySyncAdaptor(QObject *parent)
    : DropboxBackupOperationSyncAdaptor(SocialNetworkSyncAdaptor::BackupQuery, parent)
{
    setInitialActive(true);
}

DropboxBackupQuerySyncAdaptor::~DropboxBackupQuerySyncAdaptor()
{
}

DropboxBackupOperationSyncAdaptor::Operation DropboxBackupQuerySyncAdaptor::operation() const
{
    return DropboxBackupOperationSyncAdaptor::BackupQuery;
}
