/****************************************************************************
 **
 ** Copyright (C) 2013 Jolla Ltd.
 ** Contact: Chris Adams <chris.adams@jollamobile.com>
 **
 ****************************************************************************/

#include "googlecontactsyncadaptor.h"
#include "googlecontactstream.h"
#include "googlecontactatom.h"
#include "googlecontactimagedownloader.h"
#include "constants_p.h"
#include "trace.h"

#include <QtCore/QUrl>
#include <QtCore/QUrlQuery>
#include <QtCore/QFile>
#include <QtCore/QByteArray>

#include <QtContacts/QContactManager>
#include <QtContacts/QContactDetailFilter>
#include <QtContacts/QContactIntersectionFilter>
#include <QtContacts/QContact>
#include <QtContacts/QContactSyncTarget>
#include <QtContacts/QContactGuid>
#include <QtContacts/QContactName>
#include <QtContacts/QContactNickname>
#include <QtContacts/QContactAvatar>
#include <QtContacts/QContactUrl>
#include <QtContacts/QContactGender>
#include <QtContacts/QContactNote>
#include <QtContacts/QContactBirthday>
#include <QtContacts/QContactPhoneNumber>
#include <QtContacts/QContactEmailAddress>

#include <socialcache/abstractimagedownloader.h>
#include <socialcache/abstractimagedownloader_p.h>

// sailfish-components-accounts-qt5
#include <accountmanager.h>
#include <account.h>

#define SOCIALD_GOOGLE_CONTACTS_SYNCTARGET QLatin1String("google")
#define SOCIALD_GOOGLE_MAX_CONTACT_ENTRY_RESULTS 50

static const char *IMAGE_DOWNLOADER_TOKEN_KEY = "url";
static const char *IMAGE_DOWNLOADER_ACCOUNT_ID_KEY = "account_id";
static const char *IMAGE_DOWNLOADER_IDENTIFIER_KEY = "identifier";

static QContactManager *aggregatingContactManager(QObject *parent)
{
    QContactManager *retn = new QContactManager(
            QString::fromLatin1("org.nemomobile.contacts.sqlite"),
            QMap<QString, QString>(),
            parent);
    if (retn->managerName() != QLatin1String("org.nemomobile.contacts.sqlite")) {
        // the manager specified is not the aggregating manager we depend on.
        delete retn;
        return 0;
    }

    return retn;
}

GoogleContactSyncAdaptor::GoogleContactSyncAdaptor(QObject *parent)
    : GoogleDataTypeSyncAdaptor(SocialNetworkSyncAdaptor::Contacts, parent)
    , m_contactManager(aggregatingContactManager(this))
    , m_workerObject(new GoogleContactImageDownloader())
{
    setInitialActive(false);
    if (!m_contactManager) {
        TRACE(SOCIALD_ERROR,
            QString(QLatin1String("error: no aggregating contact manager exists - Google contacts sync will be inactive")));
        return;
    }

    connect(m_workerObject, &AbstractImageDownloader::imageDownloaded,
            this, &GoogleContactSyncAdaptor::imageDownloaded);

    // can sync, enabled
    setInitialActive(true);
}

GoogleContactSyncAdaptor::~GoogleContactSyncAdaptor()
{
    delete m_workerObject;
    delete m_contactManager;
}

QString GoogleContactSyncAdaptor::syncServiceName() const
{
    return QStringLiteral("google-contacts");
}

void GoogleContactSyncAdaptor::sync(const QString &dataTypeString, int accountId)
{
    // call superclass impl.
    GoogleDataTypeSyncAdaptor::sync(dataTypeString, accountId);
}

void GoogleContactSyncAdaptor::purgeDataForOldAccounts(const QList<int> &purgeIds)
{
    foreach (int pid, purgeIds) {
        // first, purge all data from QtContacts
        purgeAccount(pid);
    }
}

void GoogleContactSyncAdaptor::beginSync(int accountId, const QString &accessToken)
{
    // clear our cache lists if necessary.
    m_remoteContacts[accountId].clear();

    // begin requesting data.
    requestData(accountId, accessToken);
}

void GoogleContactSyncAdaptor::requestData(int accountId, const QString &accessToken, int startIndex, const QString &continuationRequest, const QDateTime &syncTimestamp)
{
    // note: this timestamp isn't good enough, as we store that timestamp
    // even if sync failed.  We actually need to know whether we need to
    // do a "clean + resync" or "delta" operation (and for delta, what
    // the timestamp of the most recent successful sync was).
    QDateTime timestamp = syncTimestamp.isValid() ? syncTimestamp :
                          lastSyncTimestamp(QLatin1String("google"),
                                            SocialNetworkSyncAdaptor::dataTypeName(SocialNetworkSyncAdaptor::Contacts),
                                            accountId);

    QUrl requestUrl;
    if (continuationRequest.isEmpty()) {
        requestUrl = QUrl(QString(QLatin1String("https://www.google.com/m8/feeds/contacts/default/full/")));
        QUrlQuery urlQuery;
        //For now, always query all contact information.  In the future, use the sync time as below.  TODO!
        //if (!timestamp.isNull())
        //    urlQuery.addQueryItem("updated-min", timestamp.toString(Qt::ISODate));
        if (startIndex > 1) {
            urlQuery.addQueryItem ("start-index", QString::number(startIndex));
        }
        urlQuery.addQueryItem("max-results", QString::number(SOCIALD_GOOGLE_MAX_CONTACT_ENTRY_RESULTS));
        requestUrl.setQuery(urlQuery);
    } else {
        requestUrl = QUrl(continuationRequest);
    }

    QNetworkRequest req(requestUrl);
    req.setRawHeader("GData-Version", "3.0");
    req.setRawHeader(QString(QLatin1String("Authorization")).toUtf8(),
                     QString(QLatin1String("Bearer ") + accessToken).toUtf8());

    // we're requesting data.  Increment the semaphore so that we know we're still busy.
    incrementSemaphore(accountId);
    QNetworkReply *reply = networkAccessManager->get(req);
    if (reply) {
        reply->setProperty("accountId", accountId);
        reply->setProperty("accessToken", accessToken);
        reply->setProperty("continuationRequest", continuationRequest);
        reply->setProperty("lastSyncTimestamp", timestamp);
        reply->setProperty("startIndex", startIndex);
        connect(reply, SIGNAL(finished()), this, SLOT(contactsFinishedHandler()));
        connect(reply, SIGNAL(error(QNetworkReply::NetworkError)), this, SLOT(errorHandler(QNetworkReply::NetworkError)));
        connect(reply, SIGNAL(sslErrors(QList<QSslError>)), this, SLOT(sslErrorsHandler(QList<QSslError>)));
        setupReplyTimeout(accountId, reply);
    } else {
        TRACE(SOCIALD_ERROR,
              QString(QLatin1String("error: unable to request contacts from Google account with id %1"))
              .arg(accountId));

        setStatus(SocialNetworkSyncAdaptor::Error);
        decrementSemaphore(accountId);
    }
}

void GoogleContactSyncAdaptor::contactsFinishedHandler()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    QByteArray data = reply->readAll();
    int startIndex = reply->property("startIndex").toInt();
    int accountId = reply->property("accountId").toInt();
    QString accessToken = reply->property("accessToken").toString();
    QDateTime lastSyncTimestamp = reply->property("lastSyncTimestamp").toDateTime();
    bool isError = reply->property("isError").toBool();
    reply->deleteLater();
    removeReplyTimeout(accountId, reply);

    if (isError) {
        TRACE(SOCIALD_ERROR,
              QString(QLatin1String("error occurred when performing contacts request for Google account %1"))
              .arg(accountId));

        setStatus(SocialNetworkSyncAdaptor::Error);
        decrementSemaphore(accountId);
        return;
    } else if (data.isEmpty()) {
        TRACE(SOCIALD_ERROR,
              QString(QLatin1String("error: no contact data in reply from Google with account %1"))
              .arg(accountId));

        setStatus(SocialNetworkSyncAdaptor::Error);
        decrementSemaphore(accountId);
        return;
    }

    GoogleContactStream parser(false, accountId);
    GoogleContactAtom *atom = parser.parse(data);

    if (!atom) {
        TRACE(SOCIALD_ERROR,
              QString(QLatin1String("error: unable to parse contacts data from reply from Google using account with id %1"))
              .arg(accountId));

        setStatus(SocialNetworkSyncAdaptor::Error);
        decrementSemaphore(accountId);
        return;
    }

    QList<QPair<QContact, QStringList> > remoteContacts = atom->entryContacts();
    for (int i = 0; i < remoteContacts.size(); ++i) {
        // one way sync can ignore the unsupported elements.
        m_remoteContacts[accountId].append(remoteContacts[i].first);
    }

    if (!atom->nextEntriesUrl().isEmpty()) {
        // request more if they exist.
        startIndex += SOCIALD_GOOGLE_MAX_CONTACT_ENTRY_RESULTS;
        requestData(accountId, accessToken, startIndex, atom->nextEntriesUrl(), lastSyncTimestamp);
    } else {
        // we're finished - we should attempt to update our local cache.
        int addedCount = 0, modifiedCount = 0, removedCount = 0, unchangedCount = 0;
        bool success = storeToLocal(accessToken, accountId, &addedCount, &modifiedCount, &removedCount, &unchangedCount);
        TRACE(SOCIALD_INFORMATION,
              QString(QLatin1String("Google contact sync with account %1 finished with result: %2: a: %3 m: %4 r: %5 u: %6. Continuing to load avatars..."))
              .arg(accountId).arg(success ? "SUCCESS" : "ERROR").arg(addedCount).arg(modifiedCount).arg(removedCount).arg(unchangedCount));
    }

    delete atom;
    decrementSemaphore(accountId);
}

bool GoogleContactSyncAdaptor::remoteContactDiffersFromLocal(const QContact &remoteContact, const QContact &localContact) const
{
    // check to see if there are any differences between the remote and the local.
    QList<QContactDetail> remoteDetails = remoteContact.details();
    QList<QContactDetail> localDetails = localContact.details();

    // remove any problematic details from the lists (timestamps, etc)
    for (int i = remoteDetails.size() - 1; i >= 0; --i) {
        if (remoteDetails.at(i).type() == QContactDetail::TypeTimestamp
                || remoteDetails.at(i).type() == QContactDetail::TypeDisplayLabel
                || remoteDetails.at(i).type() == QContactDetail::TypePresence
                || remoteDetails.at(i).type() == QContactDetail::TypeGlobalPresence
                || remoteDetails.at(i).type() == QContactOriginMetadata::Type) {
            remoteDetails.removeAt(i);
        }
    }
    for (int i = localDetails.size() - 1; i >= 0; --i) {
        if (localDetails.at(i).type() == QContactDetail::TypeTimestamp
                || localDetails.at(i).type() == QContactDetail::TypeDisplayLabel
                || localDetails.at(i).type() == QContactDetail::TypePresence
                || localDetails.at(i).type() == QContactDetail::TypeGlobalPresence
                || localDetails.at(i).type() == QContactOriginMetadata::Type) {
            localDetails.removeAt(i);
        }
    }

    // compare the two lists to determine if any differences exist.
    // Note: we don't just check if the count is different, because
    // sometimes we can have discardable duplicates which are detected in the backend.
    foreach (const QContactDetail &rdet, remoteDetails) {
        // find all local details of the same type
        QList<QContactDetail> localOfSameType;
        foreach (const QContactDetail &ldet, localDetails) {
            if (ldet.type() == rdet.type()) {
                localOfSameType.append(ldet);
            }
        }

        // if none exist, then the remote differs
        if (localOfSameType.isEmpty()) {
            return true;
        }

        // if none of the local are the same, the remote differs
        // note that we only ensure that the remote values have matching local values,
        // and not vice versa, as the local backend can add extra data (eg,
        // synthesised minimal/normalised phone number forms).
        bool found = false;
        foreach (const QContactDetail &ldet, localOfSameType) {
            // we only check the "default" field values, and not LinkedDetailUris.
            QMap<int, QVariant> lvalues = ldet.values();
            QMap<int, QVariant> rvalues = rdet.values();
            bool noFieldValueDifferences = true;
            foreach (int valueKey, rvalues.keys()) {
                if (ldet.type() == QContactDetail::TypePhoneNumber
                        && valueKey == QContactPhoneNumber::FieldSubTypes) {
                    // special handling for phone number sub type comparison
                    QContactPhoneNumber lph(ldet);
                    QContactPhoneNumber rph(rdet);
                    if (lph.subTypes() != rph.subTypes()) {
                        noFieldValueDifferences = false;
                        break;
                    }
                } else if (valueKey == QContactDetail::FieldContext) {
                    if (ldet.contexts() != rdet.contexts()) {
                        noFieldValueDifferences = false;
                        break;
                    }
                } else if (valueKey < QContactDetail::FieldContext) {
                    QVariant rdetv = rvalues.value(valueKey);
                    QVariant ldetv = lvalues.value(valueKey);
                    if (rdetv != ldetv) {
                        // it could be that "invalid" variant is stored
                        // as "empty" string, if the field is a string field.
                        // if so, ignore that - it's not a difference.
                        if ((!rdetv.isValid() && ldetv.type() == QVariant::String && ldetv.toString().isEmpty())
                         || (!ldetv.isValid() && rdetv.type() == QVariant::String && rdetv.toString().isEmpty())) {
                            // actually not different
                        } else {
                            // actually is different
                            noFieldValueDifferences = false;
                            break;
                        }
                    }
                }
            }
            if (noFieldValueDifferences) {
                found = true; // this detail matches.
                break;
            }
        }
        if (!found) {
            return true;
        }
    }

    // there were no differences between this remote and the local counterpart.
    return false;
}

bool GoogleContactSyncAdaptor::storeToLocal(const QString &accessToken, int accountId, int *addedCount, int *modifiedCount, int *removedCount, int *unchangedCount)
{
    // steps:
    // 1) load current data from backend
    // 2) determine delta (add/mod/rem)
    // 3) apply delta

    QContactDetailFilter syncTargetFilter;
    syncTargetFilter.setDetailType(QContactDetail::TypeSyncTarget, QContactSyncTarget::FieldSyncTarget);
    syncTargetFilter.setValue(SOCIALD_GOOGLE_CONTACTS_SYNCTARGET);
    QContactFetchHint noRelationships;
    noRelationships.setOptimizationHints(QContactFetchHint::NoRelationships);

    QList<QContact> remoteContacts = transformContactAvatars(m_remoteContacts[accountId], accountId, accessToken);
    QList<QContact> localContacts = m_contactManager->contacts(syncTargetFilter, QList<QContactSortOrder>(), noRelationships);
    QList<QContact> remoteToSave;
    QList<QContactId> localToRemove;
    QList<QContactId> foundLocal;

    // we always use the remote server's data in conflicts
    for (int i = 0; i < remoteContacts.size(); ++i) {
        QContact rc = remoteContacts[i];
        QString guid = rc.detail<QContactGuid>().guid();
        if (guid.isEmpty()) {
            TRACE(SOCIALD_ERROR,
                  QString(QLatin1String("skipping: cannot store remote google contact with no guid:")) << rc);
            continue;
        }

        bool foundLocalToModify = false;
        bool localUnchanged = false;
        for (int j = 0; j < localContacts.size(); ++j) {
            const QContact &lc = localContacts[j];
            if (lc.detail<QContactGuid>().guid() == guid) {
                // determine whether we need to update the locally stored contact at all
                if (remoteContactDiffersFromLocal(rc, lc)) {
                    // we clobber local data with remote data.
                    foundLocalToModify = true;
                    rc.setId(lc.id());
                    foundLocal.append(lc.id());
                } else {
                    // we shouldn't need to save this contact, it already exists locally.
                    localUnchanged = true;
                    rc.setId(lc.id());
                    foundLocal.append(lc.id());
                }

                break;
            }
        }

        if (localUnchanged) {
            *unchangedCount += 1;
        } else if (foundLocalToModify) {
            *modifiedCount += 1;
            remoteToSave.append(rc);
        } else {
            *addedCount += 1;
            remoteToSave.append(rc);
        }
    }

    // any local contacts which exist without a remote counterpart
    // are "stale" and should be removed.
    for (int i = 0; i < localContacts.size(); ++i) {
        QContact lc = localContacts.at(i);
        if (!foundLocal.contains(lc.id())) {
            localToRemove.append(lc.id());
            *removedCount += 1;
        }
    }

    // ensure that each of the remoteToSave have a sync target set.
    for (int i = 0; i < remoteToSave.size(); ++i) {
        QContact mod = remoteToSave.at(i);
        QContactSyncTarget gst = mod.detail<QContactSyncTarget>();
        gst.setSyncTarget(QStringLiteral("google"));
        mod.saveDetail(&gst);
        remoteToSave.replace(i, mod);
    }

    // now write the changes to the database.
    bool success = true;
    if (remoteToSave.size()) {
        success = m_contactManager->saveContacts(&remoteToSave);
        if (!success) {
            TRACE(SOCIALD_ERROR,
                  QString(QLatin1String("Failed to save contacts: %1 - with account %2"))
                  .arg(m_contactManager->error()).arg(accountId));
        }
    }
    if (localToRemove.size()) {
        success = m_contactManager->removeContacts(localToRemove);
        if (!success) {
            TRACE(SOCIALD_ERROR,
                  QString(QLatin1String("Failed to remove stale contacts: %1 - with account %2"))
                  .arg(m_contactManager->error()).arg(accountId));
        }
    }

    m_remoteContacts[accountId].clear();
    return success;
}

QList<QContact> GoogleContactSyncAdaptor::transformContactAvatars(const QList<QContact> &remoteContacts, int accountId, const QString &accessToken)
{
    // The avatar detail from the remote contact will be of the form:
    // https://www.google.com/m8/feeds/photos/media/user@gmail.com/userId
    // We need to:
    // 1) transform this to a local filename.
    // 2) determine if the local file exists.
    // 3) if not, trigger downloading the avatar.

    QList<QContact> retn;
    for (int i = 0; i < remoteContacts.size(); ++i) {
        QContact curr = remoteContacts.at(i);

        // We only deal with the first avatar from the contact.  If it has multiple,
        // then later avatars will not be transformed.  TODO: fix this.
        // We also only bother to do this for contacts with a GUID, as we don't
        // store locally any contact without one.
        if (curr.details<QContactAvatar>().size() && !curr.detail<QContactGuid>().guid().isEmpty()) {
            // we have a remote avatar which we need to transform.
            QContactAvatar avatar = curr.detail<QContactAvatar>();
            QString remoteImageUrl = avatar.imageUrl().toString();
            if (!remoteImageUrl.isEmpty() && !avatar.imageUrl().isLocalFile()) {
                QVariantMap metadata;
                metadata.insert(IMAGE_DOWNLOADER_ACCOUNT_ID_KEY, accountId);
                metadata.insert(IMAGE_DOWNLOADER_TOKEN_KEY, accessToken);
                metadata.insert(IMAGE_DOWNLOADER_IDENTIFIER_KEY, curr.detail<QContactGuid>().guid());

                // transform to a local file name.
                QString localFileName = GoogleContactImageDownloader::staticOutputFile(
                        curr.detail<QContactGuid>().guid(), remoteImageUrl);
                avatar.setImageUrl(localFileName);

                // update the value in the current contact.
                curr.saveDetail(&avatar);

                // and trigger downloading the image, if it doesn't already exist.
                // this means that we shouldn't download images needlessly after
                // first sync, but it also means that if it updates/changes on the
                // server side, we also won't retrieve any updated image.
                if (!QFile::exists(localFileName)) {
                    incrementSemaphore(accountId);
                    m_workerObject->queue(remoteImageUrl, metadata);
                }
            }
        }

        retn.append(curr);
    }

    return retn;
}

void GoogleContactSyncAdaptor::imageDownloaded(const QString &url, const QString &path,
                                               const QVariantMap &metadata)
{
    Q_UNUSED(url)
    Q_UNUSED(path)

    // Load finished, decrement semaphore
    int accountId = metadata.value(IMAGE_DOWNLOADER_ACCOUNT_ID_KEY).toInt();
    decrementSemaphore(accountId);
}

void GoogleContactSyncAdaptor::purgeAccount(int pid)
{
    int purgeCount = 0;
    int modifiedCount = 0;

    QContactDetailFilter syncTargetFilter;
    syncTargetFilter.setDetailType(QContactDetail::TypeSyncTarget, QContactSyncTarget::FieldSyncTarget);
    syncTargetFilter.setValue(SOCIALD_GOOGLE_CONTACTS_SYNCTARGET);
    QContactFetchHint noRelationships;
    noRelationships.setOptimizationHints(QContactFetchHint::NoRelationships);

    QString accountIdStr = QString::number(pid);
    QList<QContact> localContacts = m_contactManager->contacts(syncTargetFilter, QList<QContactSortOrder>(), noRelationships);
    QList<QContactId> contactsToRemove;

    // in the past, we used QContactOriginMetadata to store which account ids
    // a given contact was provided by.  Now, however, we encode the account
    // id into the QContactGuid -- so every instance of a contact from Google
    // synced from a particular account will be represented by a separate
    // QContact in the database.
    for (int i = 0; i < localContacts.size(); ++i) {
        contactsToRemove.append(localContacts[i].id());
        purgeCount += 1;
    }

    // now write the changes to the database.
    bool success = true;
    if (contactsToRemove.size()) {
        success = m_contactManager->removeContacts(contactsToRemove);
        if (!success) {
            TRACE(SOCIALD_ERROR,
                  QString(QLatin1String("Failed to remove stale contacts: %1 - during purge of account %2"))
                  .arg(m_contactManager->error()).arg(pid));
        }
    }

    if (success) {
        TRACE(SOCIALD_INFORMATION,
                QString(QLatin1String("purged account %1 and successfully removed %2 friends (kept %3 modified friends)"))
                .arg(pid).arg(purgeCount).arg(modifiedCount));
    }
}

void GoogleContactSyncAdaptor::finalCleanup()
{
    // Synchronously find any contacts which need to be removed,
    // which were somehow "left behind" by the sync process.
    // Also, determine if any avatars were not synced, and remove those details.

    // first, get a list of all existing, enabled google account ids
    QList<int> googleAccountIds;
    QList<int> purgeAccountIds;
    QList<int> currentAccountIds = accountManager->accountIdentifiers();
    foreach (int currId, currentAccountIds) {
        Account *act = accountManager->account(currId);
        if (act) {
            if (act->providerName() == QString(QLatin1String("google")) && act->enabled()
                    && act->isEnabledWithService(syncServiceName())) {
                googleAccountIds.append(currId);
            }
            act->deleteLater();
        }
    }

    // second, get all contacts which have been synced from Google.
    QContactDetailFilter syncTargetFilter;
    syncTargetFilter.setDetailType(QContactDetail::TypeSyncTarget, QContactSyncTarget::FieldSyncTarget);
    syncTargetFilter.setValue(SOCIALD_GOOGLE_CONTACTS_SYNCTARGET);
    QContactFetchHint noRelationships;
    noRelationships.setOptimizationHints(QContactFetchHint::NoRelationships);
    noRelationships.setDetailTypesHint(QList<QContactDetail::DetailType>() << QContactGuid::Type << QContactAvatar::Type);
    QList<QContact> googleContacts = m_contactManager->contacts(syncTargetFilter, QList<QContactSortOrder>(), noRelationships);

    // third, find all account ids from which contacts have been synced
    foreach (const QContact &contact, googleContacts) {
        QContactGuid guid = contact.detail<QContactGuid>();
        QString accountIdStr = guid.guid().split(':').size() ? guid.guid().split(':').first() : QString();
        if (accountIdStr.isEmpty()) {
            TRACE(SOCIALD_ERROR,
                QString(QLatin1String("finalCleanup() found Google contact with invalid Guid")));
        } else {
            int purgeId = accountIdStr.toInt();
            if (purgeId && !googleAccountIds.contains(purgeId)
                    && !purgeAccountIds.contains(purgeId)) {
                // this account no longer exists, and needs to be purged.
                purgeAccountIds.append(purgeId);
            }
        }
    }

    // fourth, remove any non-existent avatar details.
    // We save these first, in case some contacts get removed by purge.
    QList<QContact> saveList;
    for (int i = 0; i < googleContacts.size(); ++i) {
        bool contactAddedToSaveList = false;
        QContact contact = googleContacts.at(i);
        QList<QContactAvatar> allAvatars = contact.details<QContactAvatar>();
        for (int j = 0; j < allAvatars.size(); ++j) {
            QContactAvatar av = allAvatars[j];
            if (!av.imageUrl().isEmpty()) {
                // this avatar may have failed to sync.
                QUrl avatarUrl = av.imageUrl();
                if (avatarUrl.isLocalFile() && !QFile::exists(av.imageUrl().toString())) {
                    // download failed, remove it from the contact.
                    contact.removeDetail(&av);
                    if (!contactAddedToSaveList) {
                        saveList.append(contact);
                        contactAddedToSaveList = true;
                    }
                }
            }
        }
    }
    if (m_contactManager->saveContacts(&saveList)) {
        TRACE(SOCIALD_INFORMATION,
            QString(QLatin1String("finalCleanup() purged non-existent avatars from %1 Google contacts"))
            .arg(saveList.size()));
    } else {
        TRACE(SOCIALD_ERROR,
            QString(QLatin1String("finalCleanup() failed to save non-existent avatar removals for Google contacts")));
    }

    // fifth, purge all data for those account ids which no longer exist.
    if (purgeAccountIds.size()) {
        TRACE(SOCIALD_INFORMATION,
            QString(QLatin1String("finalCleanup() purging contacts from %1 non-existent Google accounts"))
            .arg(purgeAccountIds.size()));
        foreach (int purgeId, purgeAccountIds) {
            purgeAccount(purgeId);
        }
    }
}