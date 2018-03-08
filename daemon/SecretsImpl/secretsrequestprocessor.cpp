/*
 * Copyright (C) 2017 Jolla Ltd.
 * Contact: Chris Adams <chris.adams@jollamobile.com>
 * All rights reserved.
 * BSD 3-Clause License, see LICENSE.
 */

#include "secretsrequestprocessor_p.h"
#include "applicationpermissions_p.h"
#include "logging_p.h"
#include "util_p.h"
#include "plugin_p.h"

#include "Secrets/result.h"
#include "Secrets/secretmanager.h"
#include "Secrets/secret.h"
#include "Secrets/extensionplugins.h"

#include <QtCore/QPluginLoader>
#include <QtCore/QDataStream>
#include <QtCore/QVariant>
#include <QtCore/QString>
#include <QtCore/QList>
#include <QtCore/QHash>
#include <QtCore/QSet>
#include <QtCore/QDir>

using namespace Sailfish::Secrets;

// In real system, we would generate a secure key on first boot,
// and store it via a hardware-supported secure storage mechanism.
// If we ever update the secure key, we would need to decrypt all
// values stored in the secrets database with the old key, encrypt
// them with the new key, and write the updated values back to storage.
static const QByteArray SystemEncryptionKey = QByteArray("example_encryption_key");
// In real system, we would store the device lock key (hash) somewhere
// securely.  We use this device lock key to lock/unlock device-lock
// protected collections.
static const QByteArray DeviceLockKey = QByteArray("example_device_lock_key");

Daemon::ApiImpl::RequestProcessor::RequestProcessor(
        Daemon::ApiImpl::BookkeepingDatabase *bkdb,
        Daemon::ApiImpl::ApplicationPermissions *appPermissions,
        bool autotestMode,
        Daemon::ApiImpl::SecretsRequestQueue *parent)
    : QObject(parent), m_bkdb(bkdb), m_requestQueue(parent), m_appPermissions(appPermissions), m_autotestMode(autotestMode)
{
    // initialise the special "standalone" collection if needed.
    // Note that it is a "notional" collection only,
    // existing only to satisfy the database constraints.
    m_bkdb->insertCollection(QLatin1String("standalone"),
                             QLatin1String("standalone"),
                             false,
                             QLatin1String("standalone"),
                             QLatin1String("standalone"),
                             QLatin1String("standalone"),
                             0,
                             0,
                             SecretManager::OwnerOnlyMode);
}

bool
Daemon::ApiImpl::RequestProcessor::loadPlugins(const QString &pluginDir)
{
    qCDebug(lcSailfishSecretsDaemon) << "Loading plugins from directory:" << pluginDir;
    QDir dir(pluginDir);
    Q_FOREACH (const QString &pluginFile, dir.entryList(QDir::Files | QDir::NoDot | QDir::NoDotDot, QDir::Name)) {
        // load the plugin and query it for its data.
        Daemon::ApiImpl::PluginHelper loader(pluginFile, m_autotestMode);
        QObject *plugin = loader.instance();

        EncryptedStoragePlugin *encryptedStoragePlugin;
        AuthenticationPlugin *authenticationPlugin;

        if (loader.storeAs<StoragePlugin>(plugin, &m_storagePlugins, lcSailfishSecretsDaemon)) {
            // nothing more to do
        } else if (loader.failureType() != Daemon::ApiImpl::PluginHelper::PluginTypeFailure) {
            loader.reportFailure(lcSailfishSecretsDaemon);
        } else if (loader.storeAs<EncryptionPlugin>(plugin, &m_encryptionPlugins, lcSailfishSecretsDaemon)) {
            // nothing more to do
        } else if (loader.failureType() != Daemon::ApiImpl::PluginHelper::PluginTypeFailure) {
            loader.reportFailure(lcSailfishSecretsDaemon);
        } else if ((encryptedStoragePlugin = loader.storeAs<EncryptedStoragePlugin>(plugin, &m_encryptedStoragePlugins, lcSailfishSecretsDaemon))) {
            m_potentialCryptoStoragePlugins.insert(encryptedStoragePlugin->name(), plugin);
        } else if (loader.failureType() != Daemon::ApiImpl::PluginHelper::PluginTypeFailure) {
            loader.reportFailure(lcSailfishSecretsDaemon);
        } else if ((authenticationPlugin = loader.storeAs<AuthenticationPlugin>(plugin, &m_authenticationPlugins, lcSailfishSecretsDaemon))) {
            connect(authenticationPlugin, &AuthenticationPlugin::authenticationCompleted,
                    this, &Daemon::ApiImpl::RequestProcessor::authenticationCompleted);
            connect(authenticationPlugin, &AuthenticationPlugin::userInputInteractionCompleted,
                    this, &Daemon::ApiImpl::RequestProcessor::userInputInteractionCompleted);
        } else {
            loader.reportFailure(lcSailfishSecretsDaemon);
        }
    }

    return true;
}

// retrieve information about available plugins
Result
Daemon::ApiImpl::RequestProcessor::getPluginInfo(
        pid_t callerPid,
        quint64 requestId,
        QVector<StoragePluginInfo> *storagePlugins,
        QVector<EncryptionPluginInfo> *encryptionPlugins,
        QVector<EncryptedStoragePluginInfo> *encryptedStoragePlugins,
        QVector<AuthenticationPluginInfo> *authenticationPlugins)
{
    Q_UNUSED(callerPid); // TODO: perform access control request to see if the application has permission to read secure storage metadata.
    Q_UNUSED(requestId); // The request is synchronous, so don't need the requestId.

    for (const StoragePlugin *plugin : m_storagePlugins.values()) {
        storagePlugins->append(StoragePluginInfo(plugin));
    }
    for (const EncryptionPlugin *plugin : m_encryptionPlugins.values()) {
        encryptionPlugins->append(EncryptionPluginInfo(plugin));
    }
    for (const EncryptedStoragePlugin *plugin : m_encryptedStoragePlugins.values()) {
        encryptedStoragePlugins->append(EncryptedStoragePluginInfo(plugin));
    }
    for (const AuthenticationPlugin *plugin : m_authenticationPlugins.values()) {
        authenticationPlugins->append(AuthenticationPluginInfo(plugin));
    }

    return Result(Result::Succeeded);
}


Result
Daemon::ApiImpl::RequestProcessor::collectionNames(
        pid_t callerPid,
        quint64 requestId,
        QStringList *names)
{
    // TODO: perform access control request to see if the application has permission to read collection names.
    const bool applicationIsPlatformApplication = m_appPermissions->applicationIsPlatformApplication(callerPid);
    const QString callerApplicationId = applicationIsPlatformApplication
                ? m_appPermissions->platformApplicationId()
                : m_appPermissions->applicationId(callerPid);
    Q_UNUSED(requestId);
    Q_UNUSED(callerApplicationId);

    return m_bkdb->collectionNames(names);
}

// create a DeviceLock-protected collection
Result
Daemon::ApiImpl::RequestProcessor::createDeviceLockCollection(
        pid_t callerPid,
        quint64 requestId,
        const QString &collectionName,
        const QString &storagePluginName,
        const QString &encryptionPluginName,
        SecretManager::DeviceLockUnlockSemantic unlockSemantic,
        SecretManager::AccessControlMode accessControlMode)
{
    Q_UNUSED(requestId); // the request would only be asynchronous if we needed to perform the access control request, so until then it's always synchronous.

    if (collectionName.compare(QStringLiteral("standalone"), Qt::CaseInsensitive) == 0) {
        return Result(Result::InvalidCollectionError,
                      QLatin1String("Reserved collection name given"));
    } else if (storagePluginName == encryptionPluginName && !m_encryptedStoragePlugins.contains(storagePluginName)) {
        return Result(Result::InvalidExtensionPluginError,
                      QString::fromLatin1("No such encrypted storage plugin exists: %1").arg(storagePluginName));
    } else if (storagePluginName != encryptionPluginName && !m_storagePlugins.contains(storagePluginName)) {
        return Result(Result::InvalidExtensionPluginError,
                      QString::fromLatin1("No such storage plugin exists: %1").arg(storagePluginName));
    } else if (storagePluginName != encryptionPluginName && !m_encryptionPlugins.contains(encryptionPluginName)) {
        return Result(Result::InvalidExtensionPluginError,
                      QString::fromLatin1("No such encryption plugin exists: %1").arg(encryptionPluginName));
    }

    // TODO: perform access control request to see if the application has permission to write secure storage data.
    const bool applicationIsPlatformApplication = m_appPermissions->applicationIsPlatformApplication(callerPid);
    const QString callerApplicationId = applicationIsPlatformApplication
                ? m_appPermissions->platformApplicationId()
                : m_appPermissions->applicationId(callerPid);

    // Whenever we modify the bookkeeping database + perform a plugin operation,
    // we should ensure that we do it in such an order that only the bookkeeping
    // database can be "wrong", as we can correct that.
    // So, in this case, we:
    // 1) start transaction
    // 2) check that the collection name doesn't already exist, else fail
    // 3) insert the new collection entry into the master Collections table
    // 4) commit the transaction
    // 5) tell the storage plugin to create the new collection
    // 6) if (5) failed, start new transaction to remove the collection, commit.
    // In the future, we should mark the row as "dirty" via in-memory flag, if (6) fails,
    // so that we can re-attempt to remove it, at a later point in time.

    bool exists = false;
    Result existsResult = m_bkdb->collectionAlreadyExists(collectionName, &exists);
    if (existsResult.code() != Result::Succeeded) {
        return existsResult;
    } else if (exists) {
        return Result(Result::CollectionAlreadyExistsError,
                      QString::fromLatin1("Collection already exists: %1").arg(collectionName));
    }

    Result insertResult = m_bkdb->insertCollection(
                collectionName,
                callerApplicationId,
                true,
                storagePluginName,
                encryptionPluginName,
                m_autotestMode
                    ? (SecretManager::DefaultAuthenticationPluginName + QLatin1String(".test"))
                    : SecretManager::DefaultAuthenticationPluginName,
                static_cast<int>(unlockSemantic),
                0,
                accessControlMode);
    if (insertResult.code() != Result::Succeeded) {
        return insertResult;
    }

    Result pluginResult;
    if (storagePluginName == encryptionPluginName) {
        pluginResult = m_encryptedStoragePlugins[storagePluginName]->createCollection(collectionName, DeviceLockKey);
    } else {
        pluginResult = m_storagePlugins[storagePluginName]->createCollection(collectionName);
        m_collectionAuthenticationKeys.insert(collectionName, DeviceLockKey);
    }

    if (pluginResult.code() != Result::Succeeded) {
        // The plugin was unable to create the collection in its storage.  Let's delete it from our master table.
        // It may be tempting to merely remove the commitTransaction() above, and just do a rollbackTransaction() here,
        // but DO NOT do so, as that could lead to the case where the plugin->createCollection() call succeeds,
        // but the master table commit fails.
        Result cleanupResult = m_bkdb->cleanupDeleteCollection(collectionName, pluginResult);
        if (cleanupResult.code() != Result::Succeeded) {
            return cleanupResult;
        }
    }

    if (pluginResult.code() == Result::Succeeded &&
            accessControlMode == SecretManager::SystemAccessControlMode) {
        // TODO: tell AccessControl daemon to add this datum to its database.
    }

    return pluginResult;
}

// create a CustomLock-protected collection
Result
Daemon::ApiImpl::RequestProcessor::createCustomLockCollection(
        pid_t callerPid,
        quint64 requestId,
        const QString &collectionName,
        const QString &storagePluginName,
        const QString &encryptionPluginName,
        const QString &authenticationPluginName,
        SecretManager::CustomLockUnlockSemantic unlockSemantic,
        int customLockTimeoutMs,
        SecretManager::AccessControlMode accessControlMode,
        SecretManager::UserInteractionMode userInteractionMode,
        const QString &interactionServiceAddress)
{
    Q_UNUSED(requestId); // the request would only be asynchronous if we needed to perform the access control request, so until then it's always synchronous.

    if (collectionName.compare(QStringLiteral("standalone"), Qt::CaseInsensitive) == 0) {
        return Result(Result::InvalidCollectionError,
                      QLatin1String("Reserved collection name given"));
    } else if (storagePluginName == encryptionPluginName && !m_encryptedStoragePlugins.contains(storagePluginName)) {
        return Result(Result::InvalidExtensionPluginError,
                      QString::fromLatin1("No such encrypted storage plugin exists: %1").arg(storagePluginName));
    } else if (storagePluginName != encryptionPluginName && !m_storagePlugins.contains(storagePluginName)) {
        return Result(Result::InvalidExtensionPluginError,
                      QString::fromLatin1("No such storage plugin exists: %1").arg(storagePluginName));
    } else if (storagePluginName != encryptionPluginName && !m_encryptionPlugins.contains(encryptionPluginName)) {
        return Result(Result::InvalidExtensionPluginError,
                      QString::fromLatin1("No such encryption plugin exists: %1").arg(encryptionPluginName));
    } else if (!m_authenticationPlugins.contains(authenticationPluginName)) {
        return Result(Result::InvalidExtensionPluginError,
                      QString::fromLatin1("No such authentication plugin exists: %1").arg(authenticationPluginName));
    } else if (m_authenticationPlugins[authenticationPluginName]->authenticationTypes() & AuthenticationPlugin::ApplicationSpecificAuthentication
               && (userInteractionMode != SecretManager::ApplicationInteraction || interactionServiceAddress.isEmpty())) {
        return Result(Result::OperationRequiresApplicationUserInteraction,
                      QString::fromLatin1("Authentication plugin %1 requires in-process user interaction").arg(authenticationPluginName));
    } else if (userInteractionMode == SecretManager::PreventInteraction) {
        return Result(Result::OperationRequiresUserInteraction,
                      QString::fromLatin1("Authentication plugin %1 requires user interaction").arg(authenticationPluginName));
    }

    // TODO: perform access control request to see if the application has permission to write secure storage data.
    const bool applicationIsPlatformApplication = m_appPermissions->applicationIsPlatformApplication(callerPid);
    const QString callerApplicationId = applicationIsPlatformApplication
                ? m_appPermissions->platformApplicationId()
                : m_appPermissions->applicationId(callerPid);

    bool exists = false;
    Result existsResult = m_bkdb->collectionAlreadyExists(collectionName, &exists);
    if (existsResult.code() != Result::Succeeded) {
        return existsResult;
    } else if (exists) {
        return Result(Result::CollectionAlreadyExistsError,
                      QString::fromLatin1("Collection already exists: %1").arg(collectionName));
    }

    // perform the user input flow required to get the input key data which will be used
    // to encrypt the data in this collection.
    InteractionParameters ikdRequest;
    ikdRequest.setApplicationId(callerApplicationId);
    ikdRequest.setCollectionName(collectionName);
    ikdRequest.setOperation(InteractionParameters::CreateCollection);
    ikdRequest.setInputType(InteractionParameters::AlphaNumericInput);
    ikdRequest.setEchoMode(InteractionParameters::PasswordEchoOnEdit);
    ikdRequest.setPromptTrId("sailfish_secrets_create_customlock_collection_input_key_data_prompt");
    Result interactionResult = m_authenticationPlugins[authenticationPluginName]->beginUserInputInteraction(
                callerPid,
                requestId,
                ikdRequest,
                interactionServiceAddress);
    if (interactionResult.code() == Result::Failed) {
        return interactionResult;
    }

    m_pendingRequests.insert(requestId,
                             Daemon::ApiImpl::RequestProcessor::PendingRequest(
                                 callerPid,
                                 requestId,
                                 Daemon::ApiImpl::CreateCustomLockCollectionRequest,
                                 QVariantList() << collectionName
                                                << storagePluginName
                                                << encryptionPluginName
                                                << authenticationPluginName
                                                << unlockSemantic
                                                << customLockTimeoutMs
                                                << accessControlMode
                                                << userInteractionMode
                                                << interactionServiceAddress));
    return Result(Result::Pending);
}

Result
Daemon::ApiImpl::RequestProcessor::createCustomLockCollectionWithAuthenticationKey(
        pid_t callerPid,
        quint64 requestId,
        const QString &collectionName,
        const QString &storagePluginName,
        const QString &encryptionPluginName,
        const QString &authenticationPluginName,
        SecretManager::CustomLockUnlockSemantic unlockSemantic,
        int customLockTimeoutMs,
        SecretManager::AccessControlMode accessControlMode,
        SecretManager::UserInteractionMode userInteractionMode,
        const QString &interactionServiceAddress,
        const QByteArray &authenticationKey)
{
    // may be required for access control requests in the future
    Q_UNUSED(callerPid);
    Q_UNUSED(requestId);
    Q_UNUSED(userInteractionMode);
    Q_UNUSED(interactionServiceAddress);

    // TODO: perform access control request to see if the application has permission to write secure storage data.
    const bool applicationIsPlatformApplication = m_appPermissions->applicationIsPlatformApplication(callerPid);
    const QString callerApplicationId = applicationIsPlatformApplication
                ? m_appPermissions->platformApplicationId()
                : m_appPermissions->applicationId(callerPid);

    // Whenever we modify the master database + perform a plugin operation,
    // we should ensure that we do it in such an order that only the MASTER
    // database can be "wrong", as we can correct that.
    // So, in this case, we:
    // 1) start transaction
    // 2) check that the collection name doesn't already exist, else fail
    // 3) insert the new collection entry into the master Collections table
    // 4) commit the transaction
    // 5) tell the storage plugin to create the new collection
    // 6) if (5) failed, start new transaction to remove the collection, commit.
    // In the future, we should mark the row as "dirty" via in-memory flag, if (6) fails,
    // so that we can re-attempt to remove it, at a later point in time.

    // check for existence again, in case something else added it while
    // we were handling the asynchronous UI flow.
    bool exists = false;
    Result existsResult = m_bkdb->collectionAlreadyExists(collectionName, &exists);
    if (existsResult.code() != Result::Succeeded) {
        return existsResult;
    } else if (exists) {
        return Result(Result::CollectionAlreadyExistsError,
                      QString::fromLatin1("Collection already exists: %1").arg(collectionName));
    }

    Result insertResult = m_bkdb->insertCollection(
                collectionName,
                callerApplicationId,
                false,
                storagePluginName,
                encryptionPluginName,
                authenticationPluginName,
                static_cast<int>(unlockSemantic),
                customLockTimeoutMs,
                accessControlMode);
    if (insertResult.code() != Result::Succeeded) {
        return insertResult;
    }

    Result pluginResult;
    if (storagePluginName == encryptionPluginName) {
        pluginResult = m_encryptedStoragePlugins[storagePluginName]->createCollection(collectionName, authenticationKey);
    } else {
        pluginResult = m_storagePlugins[storagePluginName]->createCollection(collectionName);
        m_collectionAuthenticationKeys.insert(collectionName, authenticationKey);
        // TODO: also set CustomLockTimeoutMs, flag for "is custom key", etc.
    }

    if (pluginResult.code() == Result::Failed) {
        // The plugin was unable to create the collection in its storage.  Let's delete it from our master table.
        // It may be tempting to merely remove the commitTransaction() above, and just do a rollbackTransaction() here,
        // but DO NOT do so, as that could lead to the case where the plugin->createCollection() call succeeds,
        // but the master table commit fails.
        Result cleanupResult = m_bkdb->cleanupDeleteCollection(collectionName, pluginResult);
        if (cleanupResult.code() != Result::Succeeded) {
            return cleanupResult;
        }
    }

    if (pluginResult.code() == Result::Succeeded
            && accessControlMode == SecretManager::SystemAccessControlMode) {
        // TODO: tell AccessControl daemon to add this datum from its database.
    }

    return pluginResult;
}

// delete a collection
Result
Daemon::ApiImpl::RequestProcessor::deleteCollection(
        pid_t callerPid,
        quint64 requestId,
        const QString &collectionName,
        SecretManager::UserInteractionMode userInteractionMode)
{
    Q_UNUSED(requestId); // the request would only be asynchronous if we needed to perform the access control request, so until we implement that it's always synchronous.
    Q_UNUSED(userInteractionMode); // ditto ^

    if (collectionName.compare(QStringLiteral("standalone"), Qt::CaseInsensitive) == 0) {
        return Result(Result::InvalidCollectionError,
                      QLatin1String("Reserved collection name given"));
    } else if (collectionName.isEmpty()) {
        return Result(Result::InvalidCollectionError,
                      QLatin1String("Empty collection name given"));
    }

    // TODO: perform access control request to see if the application has permission to delete the collection.
    const bool applicationIsPlatformApplication = m_appPermissions->applicationIsPlatformApplication(callerPid);
    const QString callerApplicationId = applicationIsPlatformApplication
                ? m_appPermissions->platformApplicationId()
                : m_appPermissions->applicationId(callerPid);

    // Whenever we modify the bookkeeping database + perform a plugin operation,
    // we should ensure that we do it in such an order that only the MASTER
    // database can be "wrong", as we can correct that.
    // So, in this case, we:
    // 1) tell the storage plugin to delete the collection
    // 2) if (1) failed, return immediately
    // 3) start transaction
    // 4) delete the collection entry from the bookkeeping Collections table
    // 5) commit the transaction
    // In the future, we should mark the row as "dirty" via in-memory flag, if (5) fails,
    // so that we can re-attempt to remove it, at a later point in time.

    bool found = false;
    QString collectionApplicationId;
    QString collectionStoragePluginName;
    QString collectionEncryptionPluginName;
    SecretManager::AccessControlMode collectionAccessControlMode = SecretManager::OwnerOnlyMode;
    Result metadataResult = m_bkdb->collectionMetadata(collectionName,
                                                       &found,
                                                       &collectionApplicationId,
                                                       Q_NULLPTR,
                                                       &collectionStoragePluginName,
                                                       &collectionEncryptionPluginName,
                                                       Q_NULLPTR,
                                                       Q_NULLPTR,
                                                       Q_NULLPTR,
                                                       &collectionAccessControlMode);
    if (metadataResult.code() != Result::Succeeded) {
        return metadataResult;
    } else if (!found) {
        // return success immediately.  No such collection exists, so "deleting" succeeded.
        return Result(Result::Succeeded);
    }

    if (collectionAccessControlMode == SecretManager::SystemAccessControlMode) {
        // TODO: perform access control request, to ask for permission to delete the collection.
        return Result(Result::OperationNotSupportedError,
                      QLatin1String("Access control requests are not currently supported. TODO!"));
    } else if (collectionAccessControlMode == SecretManager::OwnerOnlyMode
               && collectionApplicationId != callerApplicationId) {
        return Result(Result::PermissionsError,
                      QString::fromLatin1("Collection %1 is owned by a different application").arg(collectionName));
    } else if (collectionStoragePluginName == collectionEncryptionPluginName
            && !m_encryptedStoragePlugins.contains(collectionStoragePluginName)) {
        // TODO: this means we have "stale" data in the database; what should we do in this case?
        return Result(Result::InvalidExtensionPluginError,
                      QString::fromLatin1("No such encrypted storage plugin exists: %1").arg(collectionStoragePluginName));
    } else if (collectionStoragePluginName != collectionEncryptionPluginName && (collectionStoragePluginName.isEmpty() || !m_storagePlugins.contains(collectionStoragePluginName))) {
        // TODO: this means we have "stale" data in the database; what should we do in this case?
        return Result(Result::InvalidExtensionPluginError,
                      QString::fromLatin1("No such storage plugin exists: %1").arg(collectionStoragePluginName));
    } else if (collectionAccessControlMode == SecretManager::OwnerOnlyMode
            && collectionApplicationId != callerApplicationId) {
        return Result(Result::PermissionsError,
                      QString::fromLatin1("Not the owner, cannot delete collection"));
    }

    Result pluginResult = collectionStoragePluginName == collectionEncryptionPluginName
            ? m_encryptedStoragePlugins[collectionStoragePluginName]->removeCollection(collectionName)
            : m_storagePlugins[collectionStoragePluginName]->removeCollection(collectionName);
    if (pluginResult.code() == Result::Failed) {
        return pluginResult;
    }

    // successfully removed from plugin storage, now remove the entry from the master table.
    m_collectionAuthenticationKeys.remove(collectionName);
    m_collectionLockTimers.remove(collectionName);
    Result deleteResult = m_bkdb->deleteCollection(collectionName);
    if (deleteResult.code() != Result::Succeeded) {
        // TODO: add a "dirty" flag for this collection somewhere in memory, so we can try again later.
        return deleteResult; // once the dirty flag is added, don't return an error here, just continue.
    }

    if (collectionAccessControlMode == SecretManager::SystemAccessControlMode) {
        // TODO: tell AccessControl daemon to remove this datum from its database.
    }

    return Result(Result::Succeeded);
}

// this method is a helper for the crypto API.
// set just the metadata for a secret in a collection.
// the actual secret will be stored directly by the crypto plugin.
Result
Daemon::ApiImpl::RequestProcessor::setCollectionSecretMetadata(
        pid_t callerPid,
        quint64 requestId,
        const Secret::Identifier &identifier)
{
    Q_UNUSED(requestId) // may be needed in the future for AccessControl.

    if (identifier.name().isEmpty()) {
        return Result(Result::InvalidSecretError,
                      QLatin1String("Empty secret name given"));
    } else if (identifier.collectionName().isEmpty()) {
        return Result(Result::InvalidCollectionError,
                      QLatin1String("Empty collection name given"));
    } else if (identifier.collectionName().compare(QStringLiteral("standalone"), Qt::CaseInsensitive) == 0) {
        return Result(Result::InvalidCollectionError,
                      QLatin1String("Reserved collection name given"));
    }

    // TODO: perform access control request to see if the application has permission to write secure storage data.
    const bool applicationIsPlatformApplication = m_appPermissions->applicationIsPlatformApplication(callerPid);
    const QString callerApplicationId = applicationIsPlatformApplication
                ? m_appPermissions->platformApplicationId()
                : m_appPermissions->applicationId(callerPid);

    bool found = false;
    QString collectionApplicationId;
    bool collectionUsesDeviceLockKey = false;
    QString collectionStoragePluginName;
    QString collectionEncryptionPluginName;
    QString collectionAuthenticationPluginName;
    int collectionUnlockSemantic = 0;
    int collectionCustomLockTimeoutMs = 0;
    SecretManager::AccessControlMode collectionAccessControlMode = SecretManager::OwnerOnlyMode;
    Result metadataResult = m_bkdb->collectionMetadata(identifier.collectionName(),
                                                       &found,
                                                       &collectionApplicationId,
                                                       &collectionUsesDeviceLockKey,
                                                       &collectionStoragePluginName,
                                                       &collectionEncryptionPluginName,
                                                       &collectionAuthenticationPluginName,
                                                       &collectionUnlockSemantic,
                                                       &collectionCustomLockTimeoutMs,
                                                       &collectionAccessControlMode);
    if (metadataResult.code() != Result::Succeeded) {
        return metadataResult;
    } else if (!found) {
        return Result(Result::InvalidCollectionError,
                      QLatin1String("Nonexistent collection name given"));
    }

    if (collectionAccessControlMode == SecretManager::SystemAccessControlMode) {
        // TODO: perform access control request, to ask for permission to set the secret in the collection.
        return Result(Result::OperationNotSupportedError,
                      QLatin1String("Access control requests are not currently supported. TODO!"));
    } else if (collectionAccessControlMode == SecretManager::OwnerOnlyMode
               && collectionApplicationId != callerApplicationId) {
        return Result(Result::PermissionsError,
                      QString::fromLatin1("Collection %1 is owned by a different application").arg(identifier.collectionName()));
    } else if (collectionStoragePluginName == collectionEncryptionPluginName
            && !m_encryptedStoragePlugins.contains(collectionStoragePluginName)) {
        // TODO: this means we have "stale" data in the database; what should we do in this case?
        return Result(Result::InvalidExtensionPluginError,
                      QString::fromLatin1("No such encrypted storage plugin exists: %1").arg(collectionStoragePluginName));
    } else if (collectionStoragePluginName != collectionEncryptionPluginName
            && (collectionStoragePluginName.isEmpty() || !m_storagePlugins.contains(collectionStoragePluginName))) {
        // TODO: this means we have "stale" data in the database; what should we do in this case?
        return Result(Result::InvalidExtensionPluginError,
                      QString::fromLatin1("No such storage plugin exists: %1").arg(collectionStoragePluginName));
    } else if (collectionStoragePluginName != collectionEncryptionPluginName
            && (collectionEncryptionPluginName.isEmpty() || !m_encryptionPlugins.contains(collectionEncryptionPluginName))) {
        // TODO: this means we have "stale" data in the database; what should we do in this case?
        return Result(Result::InvalidExtensionPluginError,
                      QString::fromLatin1("No such encryption plugin exists: %1").arg(collectionEncryptionPluginName));
    }

    // For this functionality, we require that the Crypto plugin be an EncryptedStoragePlugin.
    if (collectionStoragePluginName != collectionEncryptionPluginName) {
        // This is in the codepath for generateStoredKey()
        // where we want the key to be generated and stored by the same plugin.
        return Result(Result::InvalidExtensionPluginError,
                      QString::fromLatin1("The identified collection is not encrypted by that plugin"));
    }

    bool locked = false;
    Result pluginResult = m_encryptedStoragePlugins[collectionStoragePluginName]->isLocked(identifier.collectionName(), &locked);
    if (pluginResult.code() != Result::Succeeded) {
        return pluginResult;
    }

    if (locked) {
        // TODO: do we need an explicit unlockCollection()/relockCollection() API in Secrets?
        if (collectionUsesDeviceLockKey) {
            return Result(Result::CollectionIsLockedError,
                          QString::fromLatin1("Collection %1 is locked and requires device lock authentication").arg(identifier.collectionName()));
        }
        return Result(Result::OperationRequiresUserInteraction,
                      QString::fromLatin1("Collection %1 is locked and requires user interaction to unlock").arg(identifier.collectionName()));
    }

    bool secretAlreadyExists = false;
    const QString hashedSecretName = Daemon::Util::generateHashedSecretName(identifier.collectionName(), identifier.name());
    Result existsResult = m_bkdb->secretAlreadyExists(identifier.collectionName(),
                                                      hashedSecretName,
                                                      &secretAlreadyExists);
    if (existsResult.code() != Result::Succeeded) {
        return existsResult;
    } else if (secretAlreadyExists) {
        // Note: we return an error here, so that the Crypto API can know that it
        // does not need to perform a "deleteCollectionSecretMetadata()" request
        // if storing to the crypto plugin fails.
        return Result(Result::SecretAlreadyExistsError,
                      QString::fromLatin1("A secret with name %1 already exists in the collection %2").arg(identifier.name(), identifier.collectionName()));
    }

    return m_bkdb->insertSecret(identifier.collectionName(),
                                hashedSecretName,
                                collectionApplicationId,
                                collectionUsesDeviceLockKey,
                                collectionStoragePluginName,
                                collectionEncryptionPluginName,
                                collectionAuthenticationPluginName,
                                collectionUnlockSemantic,
                                collectionCustomLockTimeoutMs,
                                collectionAccessControlMode);
}

// this method is a helper for the crypto API.
// Delete just the metadata for a secret in a collection,
// as the actual secret was unable to be stored in the crypto plugin,
// so it doesn't exist.
Result
Daemon::ApiImpl::RequestProcessor::deleteCollectionSecretMetadata(
        pid_t callerPid,
        quint64 requestId,
        const Secret::Identifier &identifier)
{
    Q_UNUSED(callerPid)
    Q_UNUSED(requestId)

    // these checks should be unnecessary, but defensive just in case.
    if (identifier.name().isEmpty()) {
        return Result(Result::InvalidSecretError,
                      QLatin1String("Empty secret name given"));
    } else if (identifier.collectionName().isEmpty()) {
        return Result(Result::InvalidCollectionError,
                      QLatin1String("Empty collection name given"));
    } else if (identifier.collectionName().compare(QStringLiteral("standalone"), Qt::CaseInsensitive) == 0) {
        return Result(Result::InvalidCollectionError,
                      QLatin1String("Reserved collection name given"));
    }

    const QString hashedSecretName = Daemon::Util::generateHashedSecretName(identifier.collectionName(), identifier.name());
    return m_bkdb->deleteSecret(identifier.collectionName(),
                                hashedSecretName);
}

// this method is a helper for the crypto API.
// Get data from the user to use as input data to a key derivation function.
Result
Daemon::ApiImpl::RequestProcessor::userInput(
        pid_t callerPid,
        quint64 requestId,
        const InteractionParameters &uiParams)
{
    // TODO: perform access control request to see if the application has permission to request user input.
    const bool applicationIsPlatformApplication = m_appPermissions->applicationIsPlatformApplication(callerPid);
    const QString callerApplicationId = applicationIsPlatformApplication
                ? m_appPermissions->platformApplicationId()
                : m_appPermissions->applicationId(callerPid);

    QString userInputPlugin = uiParams.authenticationPluginName();
    if (uiParams.authenticationPluginName().isEmpty()) {
        // TODO: depending on type, choose the appropriate authentication plugin
        userInputPlugin = SecretManager::DefaultAuthenticationPluginName;
    }
    if (!m_authenticationPlugins.contains(userInputPlugin)) {
        return Result(Result::InvalidExtensionPluginError,
                      QString::fromLatin1("Cannot get user input from invalid authentication plugin: %1")
                      .arg(uiParams.authenticationPluginName()));
    }

    InteractionParameters ikdRequest(uiParams);
    ikdRequest.setApplicationId(callerApplicationId);
    if (ikdRequest.collectionName().isEmpty() && ikdRequest.secretName().isEmpty()) {
        // this is a request on behalf of a client application.
        // the user needs to be warned that the data they enter cannot
        // be considered to be "secure" in the secrets-storage sense.
        const QString warningPromptText = QString::fromLatin1(
                    "An application is requesting input which will be returned to the application: %1")
                .arg(ikdRequest.promptText());
        ikdRequest.setPromptText(warningPromptText);
    }
    Result interactionResult = m_authenticationPlugins[userInputPlugin]->beginUserInputInteraction(
                callerPid,
                requestId,
                ikdRequest,
                QString());
    if (interactionResult.code() == Result::Failed) {
        return interactionResult;
    }

    m_pendingRequests.insert(requestId,
                             Daemon::ApiImpl::RequestProcessor::PendingRequest(
                                 callerPid,
                                 requestId,
                                 Daemon::ApiImpl::UserInputRequest,
                                 QVariantList() << QVariant::fromValue<InteractionParameters>(ikdRequest)));
    return Result(Result::Pending);
}

// set a secret in a collection
Result
Daemon::ApiImpl::RequestProcessor::setCollectionSecret(
        pid_t callerPid,
        quint64 requestId,
        const Secret &secret,
        const Sailfish::Secrets::InteractionParameters &uiParams,
        SecretManager::UserInteractionMode userInteractionMode,
        const QString &interactionServiceAddress)
{
    if (secret.identifier().name().isEmpty()) {
        return Result(Result::InvalidSecretError,
                      QLatin1String("Empty secret name given"));
    } else if (secret.identifier().collectionName().isEmpty()) {
        return Result(Result::InvalidCollectionError,
                      QLatin1String("Empty collection name given"));
    } else if (secret.identifier().collectionName().compare(QStringLiteral("standalone"), Qt::CaseInsensitive) == 0) {
        return Result(Result::InvalidCollectionError,
                      QLatin1String("Reserved collection name given"));
    }

    // TODO: perform access control request to see if the application has permission to write secure storage data.
    const bool applicationIsPlatformApplication = m_appPermissions->applicationIsPlatformApplication(callerPid);
    const QString callerApplicationId = applicationIsPlatformApplication
                ? m_appPermissions->platformApplicationId()
                : m_appPermissions->applicationId(callerPid);

    bool found = false;
    QString collectionApplicationId;
    bool collectionUsesDeviceLockKey = false;
    QString collectionStoragePluginName;
    QString collectionEncryptionPluginName;
    QString collectionAuthenticationPluginName;
    int collectionUnlockSemantic = 0;
    int collectionCustomLockTimeoutMs = 0;
    SecretManager::AccessControlMode collectionAccessControlMode = SecretManager::OwnerOnlyMode;
    Result metadataResult = m_bkdb->collectionMetadata(secret.identifier().collectionName(),
                                                       &found,
                                                       &collectionApplicationId,
                                                       &collectionUsesDeviceLockKey,
                                                       &collectionStoragePluginName,
                                                       &collectionEncryptionPluginName,
                                                       &collectionAuthenticationPluginName,
                                                       &collectionUnlockSemantic,
                                                       &collectionCustomLockTimeoutMs,
                                                       &collectionAccessControlMode);
    if (metadataResult.code() != Result::Succeeded) {
        return metadataResult;
    } else if (!found) {
        return Result(Result::InvalidCollectionError,
                      QLatin1String("Nonexistent collection name given"));
    }

    if (collectionAccessControlMode == SecretManager::SystemAccessControlMode) {
        // TODO: perform access control request, to ask for permission to set the secret in the collection.
        return Result(Result::OperationNotSupportedError,
                      QLatin1String("Access control requests are not currently supported. TODO!"));
    } else if (collectionAccessControlMode == SecretManager::OwnerOnlyMode
               && collectionApplicationId != callerApplicationId) {
        return Result(Result::PermissionsError,
                      QString::fromLatin1("Collection %1 is owned by a different application").arg(secret.identifier().collectionName()));
    } else if (collectionStoragePluginName == collectionEncryptionPluginName
            && !m_encryptedStoragePlugins.contains(collectionStoragePluginName)) {
        // TODO: this means we have "stale" data in the database; what should we do in this case?
        return Result(Result::InvalidExtensionPluginError,
                      QString::fromLatin1("No such encrypted storage plugin exists: %1").arg(collectionStoragePluginName));
    } else if (collectionStoragePluginName != collectionEncryptionPluginName
            && (collectionStoragePluginName.isEmpty() || !m_storagePlugins.contains(collectionStoragePluginName))) {
        // TODO: this means we have "stale" data in the database; what should we do in this case?
        return Result(Result::InvalidExtensionPluginError,
                      QString::fromLatin1("No such storage plugin exists: %1").arg(collectionStoragePluginName));
    } else if (collectionStoragePluginName != collectionEncryptionPluginName
            && (collectionEncryptionPluginName.isEmpty() || !m_encryptionPlugins.contains(collectionEncryptionPluginName))) {
        // TODO: this means we have "stale" data in the database; what should we do in this case?
        return Result(Result::InvalidExtensionPluginError,
                      QString::fromLatin1("No such encryption plugin exists: %1").arg(collectionEncryptionPluginName));
    }

    // Check to see if we need to request the secret data from the user.
    if (!uiParams.isValid()) {
        // don't need to retrieve secret data from the user,
        // just store it directly.
        return setCollectionSecretGetAuthenticationKey(
                    callerPid,
                    requestId,
                    secret,
                    userInteractionMode,
                    interactionServiceAddress,
                    collectionUsesDeviceLockKey,
                    collectionApplicationId,
                    collectionStoragePluginName,
                    collectionEncryptionPluginName,
                    collectionAuthenticationPluginName,
                    collectionUnlockSemantic,
                    collectionCustomLockTimeoutMs,
                    collectionAccessControlMode);
    }

    // otherwise, we need to perform another asynchronous request,
    // to retrieve the secret data from the user.
    QString userInputPlugin = uiParams.authenticationPluginName();
    if (uiParams.authenticationPluginName().isEmpty()) {
        // TODO: depending on type, choose the appropriate authentication plugin
        userInputPlugin = SecretManager::DefaultAuthenticationPluginName;
    }
    if (!m_authenticationPlugins.contains(userInputPlugin)) {
        return Result(Result::InvalidExtensionPluginError,
                      QString::fromLatin1("Cannot get user input from invalid authentication plugin: %1")
                      .arg(uiParams.authenticationPluginName()));
    }

    // perform UI request to get the data for the secret
    InteractionParameters modifiedUiParams(uiParams);
    modifiedUiParams.setApplicationId(callerApplicationId);
    modifiedUiParams.setCollectionName(secret.identifier().collectionName());
    modifiedUiParams.setSecretName(secret.identifier().name());
    modifiedUiParams.setOperation(InteractionParameters::RequestUserData);
    modifiedUiParams.setPromptTrId(QString()); // clear it in case malicious apps try to confuse the user.
    Result authenticationResult = m_authenticationPlugins[userInputPlugin]->beginUserInputInteraction(
                callerPid,
                requestId,
                modifiedUiParams,
                interactionServiceAddress); // in most cases this last parameter will be ignored by the plugin.
    if (authenticationResult.code() == Result::Failed) {
        return authenticationResult;
    }

    m_pendingRequests.insert(requestId,
                             Daemon::ApiImpl::RequestProcessor::PendingRequest(
                                 callerPid,
                                 requestId,
                                 Daemon::ApiImpl::SetCollectionUserInputSecretRequest,
                                 QVariantList() << QVariant::fromValue<Secret>(secret)
                                                << QVariant::fromValue<InteractionParameters>(modifiedUiParams)
                                                << userInteractionMode
                                                << interactionServiceAddress
                                                << collectionUsesDeviceLockKey
                                                << collectionApplicationId
                                                << collectionStoragePluginName
                                                << collectionEncryptionPluginName
                                                << collectionAuthenticationPluginName
                                                << collectionUnlockSemantic
                                                << collectionCustomLockTimeoutMs
                                                << collectionAccessControlMode));
    return Result(Result::Pending);
}

Result
Daemon::ApiImpl::RequestProcessor::setCollectionSecretGetAuthenticationKey(
        pid_t callerPid,
        quint64 requestId,
        const Secret &secret,
        SecretManager::UserInteractionMode userInteractionMode,
        const QString &interactionServiceAddress,
        bool collectionUsesDeviceLockKey,
        const QString &collectionApplicationId,
        const QString &collectionStoragePluginName,
        const QString &collectionEncryptionPluginName,
        const QString &collectionAuthenticationPluginName,
        int collectionUnlockSemantic,
        int collectionCustomLockTimeoutMs,
        SecretManager::AccessControlMode collectionAccessControlMode)
{
    const bool applicationIsPlatformApplication = m_appPermissions->applicationIsPlatformApplication(callerPid);
    const QString callerApplicationId = applicationIsPlatformApplication
                ? m_appPermissions->platformApplicationId()
                : m_appPermissions->applicationId(callerPid);

    if (collectionStoragePluginName == collectionEncryptionPluginName) {
        bool locked = false;
        Result pluginResult = m_encryptedStoragePlugins[collectionStoragePluginName]->isLocked(secret.identifier().collectionName(), &locked);
        if (pluginResult.code() != Result::Succeeded) {
            return pluginResult;
        }
        if (!locked) {
            return setCollectionSecretWithAuthenticationKey(
                        callerPid,
                        requestId,
                        secret,
                        userInteractionMode,
                        interactionServiceAddress,
                        collectionUsesDeviceLockKey,
                        collectionApplicationId,
                        collectionStoragePluginName,
                        collectionEncryptionPluginName,
                        collectionAuthenticationPluginName,
                        collectionUnlockSemantic,
                        collectionCustomLockTimeoutMs,
                        collectionAccessControlMode,
                        QByteArray());
        }

        if (collectionUsesDeviceLockKey) {
            return Result(Result::CollectionIsLockedError,
                          QString::fromLatin1("Collection %1 is locked and requires device lock authentication").arg(secret.identifier().collectionName()));
        }

        if (userInteractionMode == SecretManager::PreventInteraction) {
            return Result(Result::OperationRequiresUserInteraction,
                          QString::fromLatin1("Authentication plugin %1 requires user interaction").arg(collectionAuthenticationPluginName));
        }

        // perform the user input flow required to get the input key data which will be used
        // to unlock this collection.
        InteractionParameters ikdRequest;
        ikdRequest.setApplicationId(callerApplicationId);
        ikdRequest.setCollectionName(secret.identifier().collectionName());
        ikdRequest.setSecretName(secret.identifier().name());
        ikdRequest.setOperation(InteractionParameters::StoreSecret);
        ikdRequest.setInputType(InteractionParameters::AlphaNumericInput);
        ikdRequest.setEchoMode(InteractionParameters::PasswordEchoOnEdit);
        ikdRequest.setPromptTrId("sailfish_secrets_store_collection_secret_input_key_data_prompt");
        Result interactionResult = m_authenticationPlugins[collectionAuthenticationPluginName]->beginUserInputInteraction(
                    callerPid,
                    requestId,
                    ikdRequest,
                    interactionServiceAddress);
        if (interactionResult.code() == Result::Failed) {
            return interactionResult;
        }

        m_pendingRequests.insert(requestId,
                                 Daemon::ApiImpl::RequestProcessor::PendingRequest(
                                     callerPid,
                                     requestId,
                                     Daemon::ApiImpl::SetCollectionSecretRequest,
                                     QVariantList() << QVariant::fromValue<Secret>(secret)
                                                    << userInteractionMode
                                                    << interactionServiceAddress
                                                    << collectionUsesDeviceLockKey
                                                    << collectionApplicationId
                                                    << collectionStoragePluginName
                                                    << collectionEncryptionPluginName
                                                    << collectionAuthenticationPluginName
                                                    << collectionUnlockSemantic
                                                    << collectionCustomLockTimeoutMs
                                                    << collectionAccessControlMode));
        return Result(Result::Pending);
    }


    if (m_collectionAuthenticationKeys.contains(secret.identifier().collectionName())) {
        return setCollectionSecretWithAuthenticationKey(
                    callerPid,
                    requestId,
                    secret,
                    userInteractionMode,
                    interactionServiceAddress,
                    collectionUsesDeviceLockKey,
                    collectionApplicationId,
                    collectionStoragePluginName,
                    collectionEncryptionPluginName,
                    collectionAuthenticationPluginName,
                    collectionUnlockSemantic,
                    collectionCustomLockTimeoutMs,
                    collectionAccessControlMode,
                    m_collectionAuthenticationKeys.value(secret.identifier().collectionName()));
    }

    if (collectionUsesDeviceLockKey) {
        return Result(Result::CollectionIsLockedError,
                      QString::fromLatin1("Collection %1 is locked and requires device lock authentication").arg(secret.identifier().collectionName()));
    }

    if (userInteractionMode == SecretManager::PreventInteraction) {
        return Result(Result::OperationRequiresUserInteraction,
                      QString::fromLatin1("Authentication plugin %1 requires user interaction").arg(collectionAuthenticationPluginName));
    }

    // perform the user input flow required to get the input key data which will be used
    // to unlock this collection.
    InteractionParameters ikdRequest;
    ikdRequest.setApplicationId(callerApplicationId);
    ikdRequest.setCollectionName(secret.identifier().collectionName());
    ikdRequest.setSecretName(secret.identifier().name());
    ikdRequest.setOperation(InteractionParameters::StoreSecret);
    ikdRequest.setInputType(InteractionParameters::AlphaNumericInput);
    ikdRequest.setEchoMode(InteractionParameters::PasswordEchoOnEdit);
    ikdRequest.setPromptTrId("sailfish_secrets_store_collection_secret_input_key_data_prompt");
    Result interactionResult = m_authenticationPlugins[collectionAuthenticationPluginName]->beginUserInputInteraction(
                callerPid,
                requestId,
                ikdRequest,
                interactionServiceAddress);
    if (interactionResult.code() == Result::Failed) {
        return interactionResult;
    }

    m_pendingRequests.insert(requestId,
                             Daemon::ApiImpl::RequestProcessor::PendingRequest(
                                 callerPid,
                                 requestId,
                                 Daemon::ApiImpl::SetCollectionSecretRequest,
                                 QVariantList() << QVariant::fromValue<Secret>(secret)
                                                << userInteractionMode
                                                << interactionServiceAddress
                                                << collectionUsesDeviceLockKey
                                                << collectionApplicationId
                                                << collectionStoragePluginName
                                                << collectionEncryptionPluginName
                                                << collectionAuthenticationPluginName
                                                << collectionUnlockSemantic
                                                << collectionCustomLockTimeoutMs
                                                << collectionAccessControlMode));
    return Result(Result::Pending);
}

Result
Daemon::ApiImpl::RequestProcessor::setCollectionSecretWithAuthenticationKey(
        pid_t callerPid,
        quint64 requestId,
        const Secret &secret,
        SecretManager::UserInteractionMode userInteractionMode,
        const QString &interactionServiceAddress,
        bool collectionUsesDeviceLockKey,
        const QString &collectionApplicationId,
        const QString &collectionStoragePluginName,
        const QString &collectionEncryptionPluginName,
        const QString &collectionAuthenticationPluginName,
        int collectionUnlockSemantic,
        int collectionCustomLockTimeoutMs,
        SecretManager::AccessControlMode collectionAccessControlMode,
        const QByteArray &authenticationKey)
{
    // In the future, we may need these for access control UI flows.
    Q_UNUSED(callerPid);
    Q_UNUSED(requestId);
    Q_UNUSED(userInteractionMode);
    Q_UNUSED(interactionServiceAddress);

    bool secretAlreadyExists = false;
    const QString hashedSecretName = Daemon::Util::generateHashedSecretName(secret.identifier().collectionName(), secret.identifier().name());
    Result existsResult = m_bkdb->secretAlreadyExists(secret.identifier().collectionName(),
                                                      hashedSecretName,
                                                      &secretAlreadyExists);
    if (existsResult.code() != Result::Succeeded) {
        return existsResult;
    } else if (!secretAlreadyExists) {
        // Write to the master database prior to the storage plugin.
        Result insertResult = m_bkdb->insertSecret(secret.identifier().collectionName(),
                                                   hashedSecretName,
                                                   collectionApplicationId,
                                                   collectionUsesDeviceLockKey,
                                                   collectionStoragePluginName,
                                                   collectionEncryptionPluginName,
                                                   collectionAuthenticationPluginName,
                                                   collectionUnlockSemantic,
                                                   collectionCustomLockTimeoutMs,
                                                   collectionAccessControlMode);
        if (insertResult.code() != Result::Succeeded) {
            return insertResult;
        }
    }

    Result pluginResult;
    if (collectionStoragePluginName == collectionEncryptionPluginName) {
        bool locked = false;
        pluginResult = m_encryptedStoragePlugins[collectionStoragePluginName]->isLocked(secret.identifier().collectionName(), &locked);
        if (pluginResult.code() == Result::Succeeded) {
            if (locked) {
                pluginResult = m_encryptedStoragePlugins[collectionStoragePluginName]->setEncryptionKey(secret.identifier().collectionName(), authenticationKey);
                if (pluginResult.code() != Result::Succeeded) {
                    // unable to apply the new authenticationKey.
                    m_encryptedStoragePlugins[collectionStoragePluginName]->setEncryptionKey(secret.identifier().collectionName(), QByteArray());
                    return Result(Result::SecretsPluginDecryptionError,
                                  QString::fromLatin1("Unable to decrypt collection %1 with the entered authentication key").arg(secret.identifier().collectionName()));

                }
                pluginResult = m_encryptedStoragePlugins[collectionStoragePluginName]->isLocked(secret.identifier().collectionName(), &locked);
                if (pluginResult.code() != Result::Succeeded) {
                    m_encryptedStoragePlugins[collectionStoragePluginName]->setEncryptionKey(secret.identifier().collectionName(), QByteArray());
                    return Result(Result::SecretsPluginDecryptionError,
                                  QString::fromLatin1("Unable to check lock state of collection %1 after setting the entered authentication key").arg(secret.identifier().collectionName()));

                }
            }
            if (locked) {
                // still locked, even after applying the new authenticationKey?  The authenticationKey was wrong.
                m_encryptedStoragePlugins[collectionStoragePluginName]->setEncryptionKey(secret.identifier().collectionName(), QByteArray());
                return Result(Result::IncorrectAuthenticationKeyError,
                              QString::fromLatin1("The authentication key entered for collection %1 was incorrect").arg(secret.identifier().collectionName()));
            } else {
                // successfully unlocked the encrypted storage collection.  write the secret.
                pluginResult = m_encryptedStoragePlugins[collectionStoragePluginName]->setSecret(secret.identifier().collectionName(), hashedSecretName, secret.identifier().name(), secret.data(), secret.filterData());
            }
        }
    } else {
        if (!m_collectionAuthenticationKeys.contains(secret.identifier().collectionName())) {
            // TODO: some way to "test" the authenticationKey!
            m_collectionAuthenticationKeys.insert(secret.identifier().collectionName(), authenticationKey);
        }

        QByteArray encrypted, encryptedName;
        pluginResult = m_encryptionPlugins[collectionEncryptionPluginName]->encryptSecret(secret.data(), m_collectionAuthenticationKeys.value(secret.identifier().collectionName()), &encrypted);
        if (pluginResult.code() == Result::Succeeded) {
            pluginResult = m_encryptionPlugins[collectionEncryptionPluginName]->encryptSecret(secret.identifier().name().toUtf8(), m_collectionAuthenticationKeys.value(secret.identifier().collectionName()), &encryptedName);
            if (pluginResult.code() == Result::Succeeded) {
                pluginResult = m_storagePlugins[collectionStoragePluginName]->setSecret(secret.identifier().collectionName(), hashedSecretName, encryptedName, encrypted, secret.filterData());
            }
        }
    }

    if (pluginResult.code() == Result::Failed && !secretAlreadyExists) {
        // The plugin was unable to set the secret in its storage.
        // Let's delete it from our master table if it was a new one.
        // It may be tempting to merely remove the commitTransaction() above, and just do a rollbackTransaction() here,
        // but DO NOT do so, as that could lead to the case where the plugin->setSecret() call succeeds,
        // but the master table commit fails.
        Result cleanupResult = m_bkdb->cleanupDeleteSecret(secret.identifier().collectionName(),
                                                           hashedSecretName,
                                                           pluginResult);
        if (cleanupResult.code() != Result::Succeeded) {
            return cleanupResult;
        }
    }

    return pluginResult;
}

// set a standalone DeviceLock-protected secret
Result
Daemon::ApiImpl::RequestProcessor::setStandaloneDeviceLockSecret(
        pid_t callerPid,
        quint64 requestId,
        const QString &storagePluginName,
        const QString &encryptionPluginName,
        const Secret &secret,
        const Sailfish::Secrets::InteractionParameters &uiParams,
        SecretManager::DeviceLockUnlockSemantic unlockSemantic,
        SecretManager::AccessControlMode accessControlMode,
        SecretManager::UserInteractionMode userInteractionMode,
        const QString &interactionServiceAddress)
{
    // TODO: Access Control requests to see if the application is permitted to set the secret.
    Q_UNUSED(requestId); // until we implement access control queries, this method is synchronous, so requestId is unused.
    Q_UNUSED(userInteractionMode);

    if (secret.identifier().name().isEmpty()) {
        return Result(Result::InvalidSecretError,
                      QLatin1String("Empty secret name given"));
    } else if (storagePluginName == encryptionPluginName && !m_encryptedStoragePlugins.contains(storagePluginName)) {
        return Result(Result::InvalidExtensionPluginError,
                      QString::fromLatin1("No such encrypted storage plugin exists: %1").arg(storagePluginName));
    } else if (storagePluginName != encryptionPluginName && !m_storagePlugins.contains(storagePluginName)) {
        return Result(Result::InvalidExtensionPluginError,
                      QString::fromLatin1("No such storage plugin exists: %1").arg(storagePluginName));
    } else if (storagePluginName != encryptionPluginName && !m_encryptionPlugins.contains(encryptionPluginName)) {
        return Result(Result::InvalidExtensionPluginError,
                      QString::fromLatin1("No such encryption plugin exists: %1").arg(encryptionPluginName));
    }

    // TODO: perform access control request to see if the application has permission to write secure storage data.
    const bool applicationIsPlatformApplication = m_appPermissions->applicationIsPlatformApplication(callerPid);
    const QString callerApplicationId = applicationIsPlatformApplication
                ? m_appPermissions->platformApplicationId()
                : m_appPermissions->applicationId(callerPid);

    bool found = false;
    QString secretApplicationId;
    bool secretUsesDeviceLockKey = false;
    QString secretStoragePluginName;
    SecretManager::AccessControlMode secretAccessControlMode = SecretManager::OwnerOnlyMode;
    const QString collectionName = QStringLiteral("standalone");
    const QString hashedSecretName = Daemon::Util::generateHashedSecretName(collectionName, secret.identifier().name());
    Result metadataResult = m_bkdb->secretMetadata(collectionName,
                                                   hashedSecretName,
                                                   &found,
                                                   &secretApplicationId,
                                                   &secretUsesDeviceLockKey,
                                                   &secretStoragePluginName,
                                                   Q_NULLPTR,
                                                   Q_NULLPTR,
                                                   Q_NULLPTR,
                                                   Q_NULLPTR,
                                                   &secretAccessControlMode);
    if (metadataResult.code() != Result::Succeeded) {
        return metadataResult;
    }

    if (found && secretAccessControlMode == SecretManager::SystemAccessControlMode) {
        // TODO: perform access control request, to ask for permission to set the secret in the collection.
        return Result(Result::OperationNotSupportedError,
                      QLatin1String("Access control requests are not currently supported. TODO!"));
    } else if (found && secretAccessControlMode == SecretManager::OwnerOnlyMode
               && secretApplicationId != callerApplicationId) {
        return Result(Result::PermissionsError,
                      QString::fromLatin1("Secret %1 is owned by a different application").arg(secret.identifier().name()));
    } else if (found && secretUsesDeviceLockKey == 0) {
        // don't update the secret if it would involve changing from a custom-lock to device-lock protected secret.
        return Result(Result::OperationNotSupportedError,
                      QString::fromLatin1("Secret %1 already exists and is not a devicelock protected secret")
                      .arg(secret.identifier().name()));
    } else if (found && secretStoragePluginName.compare(storagePluginName, Qt::CaseInsensitive) != 0) {
        // don't update the secret if it would involve changing which plugin it's stored in.
        return Result(Result::OperationNotSupportedError,
                      QString::fromLatin1("Secret %1 already exists and is not stored via plugin %2")
                      .arg(secret.identifier().name(), storagePluginName));
    }

    // If the secret data is fully specified, we don't need to request it from the user.
    if (!uiParams.isValid()) {
        return writeStandaloneDeviceLockSecret(
                    callerPid,
                    requestId,
                    callerApplicationId,
                    storagePluginName,
                    encryptionPluginName,
                    secret,
                    collectionName,
                    hashedSecretName,
                    found,
                    unlockSemantic,
                    accessControlMode);
    }

    // otherwise, we need to perform another asynchronous request,
    // to retrieve the secret data from the user.
    QString userInputPlugin = uiParams.authenticationPluginName();
    if (uiParams.authenticationPluginName().isEmpty()) {
        // TODO: depending on type, choose the appropriate authentication plugin
        userInputPlugin = SecretManager::DefaultAuthenticationPluginName;
    }
    if (!m_authenticationPlugins.contains(userInputPlugin)) {
        return Result(Result::InvalidExtensionPluginError,
                      QString::fromLatin1("Cannot get user input from invalid authentication plugin: %1")
                      .arg(uiParams.authenticationPluginName()));
    }

    // perform UI request to get the data for the secret
    InteractionParameters modifiedUiParams(uiParams);
    modifiedUiParams.setApplicationId(callerApplicationId);
    modifiedUiParams.setCollectionName(secret.identifier().collectionName());
    modifiedUiParams.setSecretName(secret.identifier().name());
    modifiedUiParams.setOperation(InteractionParameters::RequestUserData);
    modifiedUiParams.setPromptTrId(QString()); // clear it in case malicious apps try to confuse the user.
    Result authenticationResult = m_authenticationPlugins[userInputPlugin]->beginUserInputInteraction(
                callerPid,
                requestId,
                modifiedUiParams,
                interactionServiceAddress); // in most cases this last parameter will be ignored by the plugin.
    if (authenticationResult.code() == Result::Failed) {
        return authenticationResult;
    }

    m_pendingRequests.insert(requestId,
                             Daemon::ApiImpl::RequestProcessor::PendingRequest(
                                 callerPid,
                                 requestId,
                                 Daemon::ApiImpl::SetStandaloneDeviceLockUserInputSecretRequest,
                                 QVariantList() << QVariant::fromValue<Secret>(secret)
                                                << callerApplicationId
                                                << storagePluginName
                                                << encryptionPluginName
                                                << collectionName
                                                << hashedSecretName
                                                << found
                                                << unlockSemantic
                                                << accessControlMode));
    return Result(Result::Pending);
}

Result
Daemon::ApiImpl::RequestProcessor::writeStandaloneDeviceLockSecret(
        pid_t callerPid,
        quint64 requestId,
        const QString &callerApplicationId,
        const QString &storagePluginName,
        const QString &encryptionPluginName,
        const Secret &secret,
        const QString &collectionName,
        const QString &hashedSecretName,
        bool found,
        SecretManager::DeviceLockUnlockSemantic unlockSemantic,
        SecretManager::AccessControlMode accessControlMode)
{
    Q_UNUSED(callerPid) // may be required in future.
    Q_UNUSED(requestId)

    // Write to the master database prior to the storage plugin.
    Result insertUpdateResult = found
            ? m_bkdb->updateSecret(
                  collectionName,
                  hashedSecretName,
                  callerApplicationId,
                  true,
                  storagePluginName,
                  encryptionPluginName,
                  m_autotestMode
                        ? (SecretManager::DefaultAuthenticationPluginName + QLatin1String(".test"))
                        : SecretManager::DefaultAuthenticationPluginName,
                  unlockSemantic,
                  0,
                  accessControlMode)
            : m_bkdb->insertSecret(
                  collectionName,
                  hashedSecretName,
                  callerApplicationId,
                  true,
                  storagePluginName,
                  encryptionPluginName,
                  m_autotestMode
                        ? (SecretManager::DefaultAuthenticationPluginName + QLatin1String(".test"))
                        : SecretManager::DefaultAuthenticationPluginName,
                  unlockSemantic,
                  0,
                  accessControlMode);
    if (insertUpdateResult.code() != Result::Succeeded) {
        return insertUpdateResult;
    }

    Result pluginResult;
    if (storagePluginName == encryptionPluginName) {
        // TODO: does the following work?  We'd need to add methods to the encrypted storage plugin: re-encryptStandaloneSecrets or something...
        pluginResult = m_encryptedStoragePlugins[storagePluginName]->setSecret(collectionName, hashedSecretName, secret.identifier().name(), secret.data(), secret.filterData(), DeviceLockKey);
    } else {
        QByteArray encrypted, encryptedName;
        pluginResult = m_encryptionPlugins[encryptionPluginName]->encryptSecret(secret.data(), DeviceLockKey, &encrypted);
        if (pluginResult.code() == Result::Succeeded) {
            pluginResult = m_encryptionPlugins[encryptionPluginName]->encryptSecret(secret.identifier().name().toUtf8(), DeviceLockKey, &encryptedName);
            if (pluginResult.code() == Result::Succeeded) {
                pluginResult = m_storagePlugins[storagePluginName]->setSecret(collectionName, hashedSecretName, encryptedName, encrypted, secret.filterData());
                if (pluginResult.code() == Result::Succeeded) {
                    m_standaloneSecretAuthenticationKeys.insert(hashedSecretName, DeviceLockKey);
                }
            }
        }
    }

    if (pluginResult.code() == Result::Failed && !found) {
        // The plugin was unable to set the secret in its storage.
        // Let's delete it from our master table if it was a new one.
        // It may be tempting to merely remove the commitTransaction() above, and just do a rollbackTransaction() here,
        // but DO NOT do so, as that could lead to the case where the plugin->setSecret() call succeeds,
        // but the master table commit fails.
        Result cleanupResult = m_bkdb->cleanupDeleteSecret(collectionName, hashedSecretName, pluginResult);
        if (cleanupResult.code() != Result::Succeeded) {
            return cleanupResult;
        }
    }

    return pluginResult;
}

// set a standalone CustomLock-protected secret
Result
Daemon::ApiImpl::RequestProcessor::setStandaloneCustomLockSecret(
        pid_t callerPid,
        quint64 requestId,
        const QString &storagePluginName,
        const QString &encryptionPluginName,
        const QString &authenticationPluginName,
        const Secret &secret,
        const Sailfish::Secrets::InteractionParameters &uiParams,
        SecretManager::CustomLockUnlockSemantic unlockSemantic,
        int customLockTimeoutMs,
        SecretManager::AccessControlMode accessControlMode,
        SecretManager::UserInteractionMode userInteractionMode,
        const QString &interactionServiceAddress)
{
    if (secret.identifier().name().isEmpty()) {
        return Result(Result::InvalidSecretError,
                      QLatin1String("Empty secret name given"));
    } else if (storagePluginName == encryptionPluginName && !m_encryptedStoragePlugins.contains(storagePluginName)) {
        return Result(Result::InvalidExtensionPluginError,
                      QString::fromLatin1("No such encrypted storage plugin exists: %1").arg(storagePluginName));
    } else if (storagePluginName != encryptionPluginName && !m_storagePlugins.contains(storagePluginName)) {
        return Result(Result::InvalidExtensionPluginError,
                      QString::fromLatin1("No such storage plugin exists: %1").arg(storagePluginName));
    } else if (storagePluginName != encryptionPluginName && !m_encryptionPlugins.contains(encryptionPluginName)) {
        return Result(Result::InvalidExtensionPluginError,
                      QString::fromLatin1("No such encryption plugin exists: %1").arg(encryptionPluginName));
    }

    // TODO: perform access control request to see if the application has permission to write secure storage data.
    const bool applicationIsPlatformApplication = m_appPermissions->applicationIsPlatformApplication(callerPid);
    const QString callerApplicationId = applicationIsPlatformApplication
                ? m_appPermissions->platformApplicationId()
                : m_appPermissions->applicationId(callerPid);

    bool found = false;
    QString secretApplicationId;
    bool secretUsesDeviceLockKey = false;
    QString secretStoragePluginName;
    SecretManager::AccessControlMode secretAccessControlMode = SecretManager::OwnerOnlyMode;
    const QString collectionName = QStringLiteral("standalone");
    const QString hashedSecretName = Daemon::Util::generateHashedSecretName(collectionName, secret.identifier().name());
    Result metadataResult = m_bkdb->secretMetadata(collectionName,
                                                   hashedSecretName,
                                                   &found,
                                                   &secretApplicationId,
                                                   &secretUsesDeviceLockKey,
                                                   &secretStoragePluginName,
                                                   Q_NULLPTR,
                                                   Q_NULLPTR,
                                                   Q_NULLPTR,
                                                   Q_NULLPTR,
                                                   &secretAccessControlMode);
    if (metadataResult.code() != Result::Succeeded) {
        return metadataResult;
    }

    if (found && secretAccessControlMode == SecretManager::SystemAccessControlMode) {
        // TODO: perform access control request, to ask for permission to set the secret in the collection.
        return Result(Result::OperationNotSupportedError,
                      QLatin1String("Access control requests are not currently supported. TODO!"));
    } else if (found && secretAccessControlMode == SecretManager::OwnerOnlyMode
               && secretApplicationId != callerApplicationId) {
        return Result(Result::PermissionsError,
                      QString::fromLatin1("Secret %1 is owned by a different application").arg(secret.identifier().name()));
    } else if (found && secretUsesDeviceLockKey == 1) {
        // don't update the secret if it would involve changing from a device-lock to custom-lock protected secret.
        return Result(Result::OperationNotSupportedError,
                      QString::fromLatin1("Secret %1 already exists and is not a devicelock protected secret")
                      .arg(secret.identifier().name()));
    } else if (found && secretStoragePluginName.compare(storagePluginName, Qt::CaseInsensitive) != 0) {
        // don't update the secret if it would involve changing which plugin it's stored in.
        return Result(Result::OperationNotSupportedError,
                      QString::fromLatin1("Secret %1 already exists and is not stored via plugin %2")
                      .arg(secret.identifier().name(), storagePluginName));
    } else if (m_authenticationPlugins[authenticationPluginName]->authenticationTypes() & AuthenticationPlugin::ApplicationSpecificAuthentication
               && (userInteractionMode != SecretManager::ApplicationInteraction || interactionServiceAddress.isEmpty())) {
        return Result(Result::OperationRequiresApplicationUserInteraction,
                      QString::fromLatin1("Authentication plugin %1 requires in-process user interaction").arg(authenticationPluginName));
    } else if (userInteractionMode == SecretManager::PreventInteraction) {
        return Result(Result::OperationRequiresUserInteraction,
                      QString::fromLatin1("Authentication plugin %1 requires user interaction").arg(authenticationPluginName));
    }

    // If the secret data is fully specified, we don't need to request it from the user.
    if (!uiParams.isValid()) {
        return setStandaloneCustomLockSecretGetAuthenticationKey(
                    callerPid,
                    requestId,
                    callerApplicationId,
                    storagePluginName,
                    encryptionPluginName,
                    authenticationPluginName,
                    secret,
                    unlockSemantic,
                    customLockTimeoutMs,
                    accessControlMode,
                    userInteractionMode,
                    interactionServiceAddress);
    }

    // otherwise, we need to perform another asynchronous request,
    // to retrieve the secret data from the user.
    QString userInputPlugin = uiParams.authenticationPluginName();
    if (uiParams.authenticationPluginName().isEmpty()) {
        // TODO: depending on type, choose the appropriate authentication plugin
        userInputPlugin = SecretManager::DefaultAuthenticationPluginName;
    }
    if (!m_authenticationPlugins.contains(userInputPlugin)) {
        return Result(Result::InvalidExtensionPluginError,
                      QString::fromLatin1("Cannot get user input from invalid authentication plugin: %1")
                      .arg(uiParams.authenticationPluginName()));
    }

    // perform UI request to get the data for the secret
    InteractionParameters modifiedUiParams(uiParams);
    modifiedUiParams.setApplicationId(callerApplicationId);
    modifiedUiParams.setCollectionName(secret.identifier().collectionName());
    modifiedUiParams.setSecretName(secret.identifier().name());
    modifiedUiParams.setOperation(InteractionParameters::RequestUserData);
    modifiedUiParams.setPromptTrId(QString()); // clear it in case malicious apps try to confuse the user.
    Result authenticationResult = m_authenticationPlugins[userInputPlugin]->beginUserInputInteraction(
                callerPid,
                requestId,
                modifiedUiParams,
                interactionServiceAddress); // in most cases this last parameter will be ignored by the plugin.
    if (authenticationResult.code() == Result::Failed) {
        return authenticationResult;
    }

    m_pendingRequests.insert(requestId,
                             Daemon::ApiImpl::RequestProcessor::PendingRequest(
                                 callerPid,
                                 requestId,
                                 Daemon::ApiImpl::SetStandaloneCustomLockUserInputSecretRequest,
                                 QVariantList() << QVariant::fromValue<Secret>(secret)
                                                << callerApplicationId
                                                << storagePluginName
                                                << encryptionPluginName
                                                << authenticationPluginName
                                                << unlockSemantic
                                                << customLockTimeoutMs
                                                << accessControlMode
                                                << userInteractionMode
                                                << interactionServiceAddress));
    return Result(Result::Pending);
}

Result
Daemon::ApiImpl::RequestProcessor::setStandaloneCustomLockSecretGetAuthenticationKey(
        pid_t callerPid,
        quint64 requestId,
        const QString &callerApplicationId,
        const QString &storagePluginName,
        const QString &encryptionPluginName,
        const QString &authenticationPluginName,
        const Secret &secret,
        SecretManager::CustomLockUnlockSemantic unlockSemantic,
        int customLockTimeoutMs,
        SecretManager::AccessControlMode accessControlMode,
        SecretManager::UserInteractionMode userInteractionMode,
        const QString &interactionServiceAddress)
{
    // perform the user input flow required to get the input key data which will be used
    // to encrypt the secret
    InteractionParameters ikdRequest;
    ikdRequest.setApplicationId(callerApplicationId);
    ikdRequest.setCollectionName(QString());
    ikdRequest.setSecretName(secret.identifier().name());
    ikdRequest.setOperation(InteractionParameters::StoreSecret);
    ikdRequest.setInputType(InteractionParameters::AlphaNumericInput);
    ikdRequest.setEchoMode(InteractionParameters::PasswordEchoOnEdit);
    ikdRequest.setPromptTrId("sailfish_secrets_store_standalone_secret_input_key_data_prompt");
    Result interactionResult = m_authenticationPlugins[authenticationPluginName]->beginUserInputInteraction(
                callerPid,
                requestId,
                ikdRequest,
                interactionServiceAddress);
    if (interactionResult.code() == Result::Failed) {
        return interactionResult;
    }

    m_pendingRequests.insert(requestId,
                             Daemon::ApiImpl::RequestProcessor::PendingRequest(
                                 callerPid,
                                 requestId,
                                 Daemon::ApiImpl::SetStandaloneCustomLockSecretRequest,
                                 QVariantList() << storagePluginName
                                                << encryptionPluginName
                                                << authenticationPluginName
                                                << QVariant::fromValue<Secret>(secret)
                                                << unlockSemantic
                                                << customLockTimeoutMs
                                                << accessControlMode
                                                << userInteractionMode
                                                << interactionServiceAddress));
    return Result(Result::Pending);
}

Result
Daemon::ApiImpl::RequestProcessor::setStandaloneCustomLockSecretWithAuthenticationKey(
        pid_t callerPid,
        quint64 requestId,
        const QString &storagePluginName,
        const QString &encryptionPluginName,
        const QString &authenticationPluginName,
        const Secret &secret,
        SecretManager::CustomLockUnlockSemantic unlockSemantic,
        int customLockTimeoutMs,
        SecretManager::AccessControlMode accessControlMode,
        SecretManager::UserInteractionMode userInteractionMode,
        const QString &interactionServiceAddress,
        const QByteArray &authenticationKey)
{
    Q_UNUSED(requestId);
    Q_UNUSED(userInteractionMode);
    Q_UNUSED(interactionServiceAddress);

    // TODO: perform access control request to see if the application has permission to write secure storage data.
    const bool applicationIsPlatformApplication = m_appPermissions->applicationIsPlatformApplication(callerPid);
    const QString callerApplicationId = applicationIsPlatformApplication
                ? m_appPermissions->platformApplicationId()
                : m_appPermissions->applicationId(callerPid);

    // check again in case some other application added the secret while
    // the asynchronous authentication plugin request was active.
    bool found = false;
    QString secretApplicationId;
    bool secretUsesDeviceLockKey = false;
    QString secretStoragePluginName;
    SecretManager::AccessControlMode secretAccessControlMode = SecretManager::OwnerOnlyMode;
    const QString collectionName = QStringLiteral("standalone");
    const QString hashedSecretName = Daemon::Util::generateHashedSecretName(collectionName, secret.identifier().name());
    Result metadataResult = m_bkdb->secretMetadata(collectionName,
                                                   hashedSecretName,
                                                   &found,
                                                   &secretApplicationId,
                                                   &secretUsesDeviceLockKey,
                                                   &secretStoragePluginName,
                                                   Q_NULLPTR,
                                                   Q_NULLPTR,
                                                   Q_NULLPTR,
                                                   Q_NULLPTR,
                                                   &secretAccessControlMode);
    if (metadataResult.code() != Result::Succeeded) {
        return metadataResult;
    }

    if (found && secretAccessControlMode == SecretManager::SystemAccessControlMode) {
        // TODO: perform access control request, to ask for permission to set the secret in the collection.
        return Result(Result::OperationNotSupportedError,
                      QLatin1String("Access control requests are not currently supported. TODO!"));
    } else if (found && secretAccessControlMode == SecretManager::OwnerOnlyMode
               && secretApplicationId != callerApplicationId) {
        return Result(Result::PermissionsError,
                      QString::fromLatin1("Secret %1 is owned by a different application").arg(secret.identifier().name()));
    } else if (found && secretUsesDeviceLockKey == 1) {
        // don't update the secret if it would involve changing from a device-lock to custom-lock protected secret.
        return Result(Result::OperationNotSupportedError,
                      QString::fromLatin1("Secret %1 already exists and is not a devicelock protected secret")
                      .arg(secret.identifier().name()));
    } else if (found && secretStoragePluginName.compare(storagePluginName, Qt::CaseInsensitive) != 0) {
        // don't update the secret if it would involve changing which plugin it's stored in.
        return Result(Result::OperationNotSupportedError,
                      QString::fromLatin1("Secret %1 already exists and is not stored via plugin %2")
                      .arg(secret.identifier().name(), storagePluginName));
    }

    // Write to the master database prior to the storage plugin.
    Result insertUpdateResult = found
            ? m_bkdb->updateSecret(
                  collectionName,
                  hashedSecretName,
                  callerApplicationId,
                  false,
                  storagePluginName,
                  encryptionPluginName,
                  authenticationPluginName,
                  unlockSemantic,
                  customLockTimeoutMs,
                  accessControlMode)
            : m_bkdb->insertSecret(
                  collectionName,
                  hashedSecretName,
                  callerApplicationId,
                  false,
                  storagePluginName,
                  encryptionPluginName,
                  authenticationPluginName,
                  unlockSemantic,
                  customLockTimeoutMs,
                  accessControlMode);
    if (insertUpdateResult.code() != Result::Succeeded) {
        return insertUpdateResult;
    }

    Result pluginResult;
    if (storagePluginName == encryptionPluginName) {
        // TODO: does the following work?  We'd need to add methods to the encrypted storage plugin: re-encryptStandaloneSecrets or something...
        pluginResult = m_encryptedStoragePlugins[storagePluginName]->setSecret(collectionName, hashedSecretName, secret.identifier().name(), secret.data(), secret.filterData(), authenticationKey);
    } else {
        QByteArray encrypted, encryptedName;
        pluginResult = m_encryptionPlugins[encryptionPluginName]->encryptSecret(secret.data(), authenticationKey, &encrypted);
        if (pluginResult.code() == Result::Succeeded) {
            pluginResult = m_encryptionPlugins[encryptionPluginName]->encryptSecret(secret.identifier().name().toUtf8(), authenticationKey, &encryptedName);
            if (pluginResult.code() == Result::Succeeded) {
                pluginResult = m_storagePlugins[storagePluginName]->setSecret(collectionName, hashedSecretName, encryptedName, encrypted, secret.filterData());
                if (pluginResult.code() == Result::Succeeded) {
                    m_standaloneSecretAuthenticationKeys.insert(hashedSecretName, authenticationKey);
                }
            }
        }
    }

    if (pluginResult.code() == Result::Failed && !found) {
        // The plugin was unable to set the secret in its storage.
        // Let's delete it from our master table if it was a new one.
        // It may be tempting to merely remove the commitTransaction() above, and just do a rollbackTransaction() here,
        // but DO NOT do so, as that could lead to the case where the plugin->setSecret() call succeeds,
        // but the master table commit fails.
        Result cleanupResult = m_bkdb->cleanupDeleteSecret(collectionName, hashedSecretName, pluginResult);
        if (cleanupResult.code() != Result::Succeeded) {
            return cleanupResult;
        }
    }

    return pluginResult;
}

// get a secret in a collection
Result
Daemon::ApiImpl::RequestProcessor::getCollectionSecret(
        pid_t callerPid,
        quint64 requestId,
        const Secret::Identifier &identifier,
        SecretManager::UserInteractionMode userInteractionMode,
        const QString &interactionServiceAddress,
        Secret *secret)
{
    if (identifier.name().isEmpty()) {
        return Result(Result::InvalidSecretError,
                      QLatin1String("Empty secret name given"));
    } else if (identifier.collectionName().isEmpty()) {
        return Result(Result::InvalidCollectionError,
                      QLatin1String("Empty collection name given"));
    } else if (identifier.collectionName().compare(QStringLiteral("standalone"), Qt::CaseInsensitive) == 0) {
        return Result(Result::InvalidCollectionError,
                      QLatin1String("Reserved collection name given"));
    }

    // TODO: perform access control request to see if the application has permission to write secure storage data.
    const bool applicationIsPlatformApplication = m_appPermissions->applicationIsPlatformApplication(callerPid);
    const QString callerApplicationId = applicationIsPlatformApplication
                ? m_appPermissions->platformApplicationId()
                : m_appPermissions->applicationId(callerPid);

    bool found = false;
    QString collectionApplicationId;
    bool collectionUsesDeviceLockKey = false;
    QString collectionStoragePluginName;
    QString collectionEncryptionPluginName;
    QString collectionAuthenticationPluginName;
    int collectionUnlockSemantic = 0;
    int collectionCustomLockTimeoutMs = 0;
    SecretManager::AccessControlMode collectionAccessControlMode = SecretManager::OwnerOnlyMode;
    Result metadataResult = m_bkdb->collectionMetadata(identifier.collectionName(),
                                                       &found,
                                                       &collectionApplicationId,
                                                       &collectionUsesDeviceLockKey,
                                                       &collectionStoragePluginName,
                                                       &collectionEncryptionPluginName,
                                                       &collectionAuthenticationPluginName,
                                                       &collectionUnlockSemantic,
                                                       &collectionCustomLockTimeoutMs,
                                                       &collectionAccessControlMode);
    if (metadataResult.code() != Result::Succeeded) {
        return metadataResult;
    } else if (!found) {
        return Result(Result::InvalidCollectionError,
                      QLatin1String("Nonexistent collection name given"));
    }

    if (collectionStoragePluginName == collectionEncryptionPluginName && !m_encryptedStoragePlugins.contains(collectionStoragePluginName)) {
        // TODO: stale data, plugin was removed but data still exists...?
        return Result(Result::InvalidExtensionPluginError,
                      QString::fromLatin1("No such encrypted storage plugin exists: %1").arg(collectionStoragePluginName));
    } else if (collectionStoragePluginName != collectionEncryptionPluginName && !m_storagePlugins.contains(collectionStoragePluginName)) {
        // TODO: stale data, plugin was removed but data still exists...?
        return Result(Result::InvalidExtensionPluginError,
                      QString::fromLatin1("No such storage plugin exists: %1").arg(collectionStoragePluginName));
    } else if (collectionStoragePluginName != collectionEncryptionPluginName && !m_encryptionPlugins.contains(collectionEncryptionPluginName)) {
        // TODO: stale data, plugin was removed but data still exists...?
        return Result(Result::InvalidExtensionPluginError,
                      QString::fromLatin1("No such encryption plugin exists: %1").arg(collectionEncryptionPluginName));
    } else if (collectionAccessControlMode == SecretManager::SystemAccessControlMode) {
        // TODO: perform access control request, to ask for permission to set the secret in the collection.
        return Result(Result::OperationNotSupportedError,
                      QLatin1String("Access control requests are not currently supported. TODO!"));
    } else if (collectionAccessControlMode == SecretManager::OwnerOnlyMode
               && collectionApplicationId != callerApplicationId) {
        return Result(Result::PermissionsError,
                      QString::fromLatin1("Collection %1 is owned by a different application").arg(identifier.collectionName()));
    } else if (!m_authenticationPlugins.contains(collectionAuthenticationPluginName)) {
        return Result(Result::InvalidExtensionPluginError,
                      QString::fromLatin1("No such authentication plugin available: %1").arg(collectionAuthenticationPluginName));
    }

    if (collectionStoragePluginName == collectionEncryptionPluginName) {
        bool locked = false;
        Result pluginResult = m_encryptedStoragePlugins[collectionStoragePluginName]->isLocked(identifier.collectionName(), &locked);
        if (pluginResult.code() != Result::Succeeded) {
            return pluginResult;
        }

        if (locked) {
            if (collectionUsesDeviceLockKey) {
                return Result(Result::CollectionIsLockedError,
                              QString::fromLatin1("Collection %1 is locked and requires device lock authentication").arg(identifier.collectionName()));
            } else {
                if (userInteractionMode == SecretManager::PreventInteraction) {
                    return Result(Result::OperationRequiresUserInteraction,
                                  QString::fromLatin1("Authentication plugin %1 requires user interaction").arg(collectionAuthenticationPluginName));
                } else if (m_authenticationPlugins[collectionAuthenticationPluginName]->authenticationTypes() & AuthenticationPlugin::ApplicationSpecificAuthentication
                            && (userInteractionMode != SecretManager::ApplicationInteraction || interactionServiceAddress.isEmpty())) {
                    return Result(Result::OperationRequiresApplicationUserInteraction,
                                  QString::fromLatin1("Authentication plugin %1 requires in-process user interaction").arg(collectionAuthenticationPluginName));
                }

                // perform the user input flow required to get the input key data which will be used
                // to unlock the collection.
                InteractionParameters ikdRequest;
                ikdRequest.setApplicationId(callerApplicationId);
                ikdRequest.setCollectionName(identifier.collectionName());
                ikdRequest.setSecretName(identifier.name());
                ikdRequest.setOperation(InteractionParameters::ReadSecret);
                ikdRequest.setInputType(InteractionParameters::AlphaNumericInput);
                ikdRequest.setEchoMode(InteractionParameters::PasswordEchoOnEdit);
                ikdRequest.setPromptTrId("sailfish_secrets_get_collection_secret_input_key_data_prompt");
                Result interactionResult = m_authenticationPlugins[collectionAuthenticationPluginName]->beginUserInputInteraction(
                            callerPid,
                            requestId,
                            ikdRequest,
                            interactionServiceAddress);
                if (interactionResult.code() == Result::Failed) {
                    return interactionResult;
                }

                m_pendingRequests.insert(requestId,
                                         Daemon::ApiImpl::RequestProcessor::PendingRequest(
                                             callerPid,
                                             requestId,
                                             Daemon::ApiImpl::GetCollectionSecretRequest,
                                             QVariantList() << QVariant::fromValue<Secret::Identifier>(identifier)
                                                            << userInteractionMode
                                                            << interactionServiceAddress
                                                            << collectionStoragePluginName
                                                            << collectionEncryptionPluginName
                                                            << collectionUnlockSemantic
                                                            << collectionCustomLockTimeoutMs));
                return Result(Result::Pending);
            }
        } else {
            return getCollectionSecretWithAuthenticationKey(
                        callerPid,
                        requestId,
                        identifier,
                        userInteractionMode,
                        interactionServiceAddress,
                        collectionStoragePluginName,
                        collectionEncryptionPluginName,
                        collectionUnlockSemantic,
                        collectionCustomLockTimeoutMs,
                        QByteArray(), // no key required, it's unlocked already.
                        secret);
        }
    } else {
        if (!m_collectionAuthenticationKeys.contains(identifier.collectionName())) {
            if (collectionUsesDeviceLockKey) {
                return Result(Result::CollectionIsLockedError,
                              QString::fromLatin1("Collection %1 is locked and requires device lock authentication").arg(identifier.collectionName()));
            } else {
                if (userInteractionMode == SecretManager::PreventInteraction) {
                    return Result(Result::OperationRequiresUserInteraction,
                                  QString::fromLatin1("Authentication plugin %1 requires user interaction").arg(collectionAuthenticationPluginName));
                } else if (m_authenticationPlugins[collectionAuthenticationPluginName]->authenticationTypes() & AuthenticationPlugin::ApplicationSpecificAuthentication
                           && (userInteractionMode != SecretManager::ApplicationInteraction || interactionServiceAddress.isEmpty())) {
                    return Result(Result::OperationRequiresApplicationUserInteraction,
                                  QString::fromLatin1("Authentication plugin %1 requires in-process user interaction").arg(collectionAuthenticationPluginName));
                }

                // perform the user input flow required to get the input key data which will be used
                // to unlock the collection.
                InteractionParameters ikdRequest;
                ikdRequest.setApplicationId(callerApplicationId);
                ikdRequest.setCollectionName(identifier.collectionName());
                ikdRequest.setSecretName(identifier.name());
                ikdRequest.setOperation(InteractionParameters::ReadSecret);
                ikdRequest.setInputType(InteractionParameters::AlphaNumericInput);
                ikdRequest.setEchoMode(InteractionParameters::PasswordEchoOnEdit);
                ikdRequest.setPromptTrId("sailfish_secrets_get_collection_secret_input_key_data_prompt");
                Result interactionResult = m_authenticationPlugins[collectionAuthenticationPluginName]->beginUserInputInteraction(
                            callerPid,
                            requestId,
                            ikdRequest,
                            interactionServiceAddress);
                if (interactionResult.code() == Result::Failed) {
                    return interactionResult;
                }

                m_pendingRequests.insert(requestId,
                                         Daemon::ApiImpl::RequestProcessor::PendingRequest(
                                             callerPid,
                                             requestId,
                                             Daemon::ApiImpl::GetCollectionSecretRequest,
                                             QVariantList() << QVariant::fromValue<Secret::Identifier>(identifier)
                                                            << userInteractionMode
                                                            << interactionServiceAddress
                                                            << collectionStoragePluginName
                                                            << collectionEncryptionPluginName
                                                            << collectionUnlockSemantic
                                                            << collectionCustomLockTimeoutMs));
                return Result(Result::Pending);
            }
        } else {
            return getCollectionSecretWithAuthenticationKey(
                        callerPid,
                        requestId,
                        identifier,
                        userInteractionMode,
                        interactionServiceAddress,
                        collectionStoragePluginName,
                        collectionEncryptionPluginName,
                        collectionUnlockSemantic,
                        collectionCustomLockTimeoutMs,
                        m_collectionAuthenticationKeys.value(identifier.collectionName()),
                        secret);
        }
    }
}

Result
Daemon::ApiImpl::RequestProcessor::getCollectionSecretWithAuthenticationKey(
        pid_t callerPid,
        quint64 requestId,
        const Secret::Identifier &identifier,
        SecretManager::UserInteractionMode userInteractionMode,
        const QString &interactionServiceAddress,
        const QString &storagePluginName,
        const QString &encryptionPluginName,
        int collectionUnlockSemantic,
        int collectionCustomLockTimeoutMs,
        const QByteArray &authenticationKey,
        Secret *secret)
{
    // might be required in future for access control requests.
    Q_UNUSED(callerPid);
    Q_UNUSED(requestId);
    Q_UNUSED(userInteractionMode);
    Q_UNUSED(interactionServiceAddress);

    if (collectionUnlockSemantic == SecretManager::CustomLockTimoutRelock) {
        if (!m_collectionLockTimers.contains(identifier.collectionName())) {
            QTimer *timer = new QTimer(this);
            connect(timer, &QTimer::timeout,
                    this, &Daemon::ApiImpl::RequestProcessor::timeoutRelockCollection);
            timer->setInterval(collectionCustomLockTimeoutMs);
            timer->setSingleShot(true);
            timer->start();
            m_collectionLockTimers.insert(identifier.collectionName(), timer);
        }
    }

    const QString hashedSecretName = Daemon::Util::generateHashedSecretName(identifier.collectionName(), identifier.name());
    Result pluginResult;
    if (storagePluginName == encryptionPluginName) {
        bool locked = false;
        pluginResult = m_encryptedStoragePlugins[storagePluginName]->isLocked(identifier.collectionName(), &locked);
        if (pluginResult.code() != Result::Succeeded) {
            return pluginResult;
        }
        // if it's locked, attempt to unlock it
        if (locked) {
            pluginResult = m_encryptedStoragePlugins[storagePluginName]->setEncryptionKey(identifier.collectionName(), authenticationKey);
            if (pluginResult.code() != Result::Succeeded) {
                // unable to apply the new authenticationKey.
                m_encryptedStoragePlugins[storagePluginName]->setEncryptionKey(identifier.collectionName(), QByteArray());
                return Result(Result::SecretsPluginDecryptionError,
                              QString::fromLatin1("Unable to decrypt collection %1 with the entered authentication key").arg(identifier.collectionName()));

            }
            pluginResult = m_encryptedStoragePlugins[storagePluginName]->isLocked(identifier.collectionName(), &locked);
            if (pluginResult.code() != Result::Succeeded) {
                m_encryptedStoragePlugins[storagePluginName]->setEncryptionKey(identifier.collectionName(), QByteArray());
                return Result(Result::SecretsPluginDecryptionError,
                              QString::fromLatin1("Unable to check lock state of collection %1 after setting the entered authentication key").arg(identifier.collectionName()));

            }
        }
        if (locked) {
            // still locked, even after applying the new authenticationKey?  The authenticationKey was wrong.
            m_encryptedStoragePlugins[storagePluginName]->setEncryptionKey(identifier.collectionName(), QByteArray());
            return Result(Result::IncorrectAuthenticationKeyError,
                          QString::fromLatin1("The authentication key entered for collection %1 was incorrect").arg(identifier.collectionName()));
        }
        // successfully unlocked the encrypted storage collection.  read the secret.
        QString secretName;
        QByteArray secretData;
        Secret::FilterData secretFilterdata;
        pluginResult = m_encryptedStoragePlugins[storagePluginName]->getSecret(identifier.collectionName(), hashedSecretName, &secretName, &secretData, &secretFilterdata);
        secret->setData(secretData);
        secret->setFilterData(secretFilterdata);
    } else {
        if (!m_collectionAuthenticationKeys.contains(identifier.collectionName())) {
            // TODO: some way to "test" the authenticationKey!  also, if it's a custom lock, set the timeout, etc.
            m_collectionAuthenticationKeys.insert(identifier.collectionName(), authenticationKey);
        }

        QByteArray encrypted, encryptedName;
        Secret::FilterData filterData;
        pluginResult = m_storagePlugins[storagePluginName]->getSecret(identifier.collectionName(), hashedSecretName, &encryptedName, &encrypted, &filterData);
        if (pluginResult.code() == Result::Succeeded) {
            QByteArray decrypted;
            pluginResult = m_encryptionPlugins[encryptionPluginName]->decryptSecret(encrypted, m_collectionAuthenticationKeys.value(identifier.collectionName()), &decrypted);
            secret->setData(decrypted);
            secret->setIdentifier(identifier);
            secret->setFilterData(filterData);
        }
    }

    return pluginResult;
}

// get a standalone secret
Result
Daemon::ApiImpl::RequestProcessor::getStandaloneSecret(
        pid_t callerPid,
        quint64 requestId,
        const Secret::Identifier &identifier,
        SecretManager::UserInteractionMode userInteractionMode,
        const QString &interactionServiceAddress,
        Secret *secret)
{
    if (identifier.name().isEmpty()) {
        return Result(Result::InvalidSecretError,
                      QLatin1String("Empty secret name given"));
    } else if (!identifier.collectionName().isEmpty()) {
        return Result(Result::InvalidCollectionError,
                      QLatin1String("Non-empty collection given for standalone secret request"));
    }

    // TODO: perform access control request to see if the application has permission to write secure storage data.
    const bool applicationIsPlatformApplication = m_appPermissions->applicationIsPlatformApplication(callerPid);
    const QString callerApplicationId = applicationIsPlatformApplication
                ? m_appPermissions->platformApplicationId()
                : m_appPermissions->applicationId(callerPid);

    bool found = false;
    QString secretApplicationId;
    bool secretUsesDeviceLockKey = false;
    QString secretStoragePluginName;
    QString secretEncryptionPluginName;
    QString secretAuthenticationPluginName;
    int secretUnlockSemantic = 0;
    int secretCustomLockTimeoutMs = 0;
    SecretManager::AccessControlMode secretAccessControlMode = SecretManager::OwnerOnlyMode;
    const QString collectionName = QStringLiteral("standalone");
    const QString hashedSecretName = Daemon::Util::generateHashedSecretName(collectionName, identifier.name());
    Result metadataResult = m_bkdb->secretMetadata(
                collectionName,
                hashedSecretName,
                &found,
                &secretApplicationId,
                &secretUsesDeviceLockKey,
                &secretStoragePluginName,
                &secretEncryptionPluginName,
                &secretAuthenticationPluginName,
                &secretUnlockSemantic,
                &secretCustomLockTimeoutMs,
                &secretAccessControlMode);
    if (metadataResult.code() != Result::Succeeded) {
        return metadataResult;
    } else if (!found) {
        return Result(Result::InvalidSecretError,
                      QLatin1String("Nonexistent secret name given"));
    }

    if (secretStoragePluginName == secretEncryptionPluginName && !m_encryptedStoragePlugins.contains(secretStoragePluginName)) {
        // TODO: stale data, plugin was removed but data still exists...?
        return Result(Result::InvalidExtensionPluginError,
                      QString::fromLatin1("No such encrypted storage plugin exists: %1").arg(secretStoragePluginName));
    } else if (secretStoragePluginName != secretEncryptionPluginName && !m_storagePlugins.contains(secretStoragePluginName)) {
        // TODO: stale data, plugin was removed but data still exists...?
        return Result(Result::InvalidExtensionPluginError,
                      QString::fromLatin1("No such storage plugin exists: %1").arg(secretStoragePluginName));
    } else if (secretStoragePluginName != secretEncryptionPluginName && !m_encryptionPlugins.contains(secretEncryptionPluginName)) {
        // TODO: stale data, plugin was removed but data still exists...?
        return Result(Result::InvalidExtensionPluginError,
                      QString::fromLatin1("No such encryption plugin exists: %1").arg(secretEncryptionPluginName));
    } else if (secretAccessControlMode == SecretManager::SystemAccessControlMode) {
        // TODO: perform access control request, to ask for permission to set the secret.
        return Result(Result::OperationNotSupportedError,
                      QLatin1String("Access control requests are not currently supported. TODO!"));
    } else if (secretAccessControlMode == SecretManager::OwnerOnlyMode
               && secretApplicationId != callerApplicationId) {
        return Result(Result::PermissionsError,
                      QString::fromLatin1("Secret %1 is owned by a different application").arg(identifier.name()));
    } else if (m_authenticationPlugins[secretAuthenticationPluginName]->authenticationTypes() & AuthenticationPlugin::ApplicationSpecificAuthentication
               && (userInteractionMode != SecretManager::ApplicationInteraction || interactionServiceAddress.isEmpty())) {
        return Result(Result::OperationRequiresApplicationUserInteraction,
                      QString::fromLatin1("Authentication plugin %1 requires in-process user interaction").arg(secretAuthenticationPluginName));
    }

    if (m_standaloneSecretAuthenticationKeys.contains(hashedSecretName)) {
        return getStandaloneSecretWithAuthenticationKey(
                    callerPid,
                    requestId,
                    identifier,
                    userInteractionMode,
                    interactionServiceAddress,
                    secretStoragePluginName,
                    secretEncryptionPluginName,
                    secretUnlockSemantic,
                    secretCustomLockTimeoutMs,
                    m_standaloneSecretAuthenticationKeys.value(hashedSecretName),
                    secret);
    }

    if (secretUsesDeviceLockKey) {
        return Result(Result::CollectionIsLockedError,
                      QString::fromLatin1("Secret %1 is locked and requires device lock authentication").arg(identifier.name()));
    }

    if (userInteractionMode == SecretManager::PreventInteraction) {
        return Result(Result::OperationRequiresUserInteraction,
                      QString::fromLatin1("Authentication plugin %1 requires user interaction").arg(secretAuthenticationPluginName));
    }

    // perform the user input flow required to get the input key data which will be used
    // to decrypt the secret.
    InteractionParameters ikdRequest;
    ikdRequest.setApplicationId(callerApplicationId);
    ikdRequest.setCollectionName(QString());
    ikdRequest.setSecretName(identifier.name());
    ikdRequest.setOperation(InteractionParameters::ReadSecret);
    ikdRequest.setInputType(InteractionParameters::AlphaNumericInput);
    ikdRequest.setEchoMode(InteractionParameters::PasswordEchoOnEdit);
    ikdRequest.setPromptTrId("sailfish_secrets_get_standalone_secret_input_key_data_prompt");
    Result interactionResult = m_authenticationPlugins[secretAuthenticationPluginName]->beginUserInputInteraction(
                callerPid,
                requestId,
                ikdRequest,
                interactionServiceAddress);
    if (interactionResult.code() == Result::Failed) {
        return interactionResult;
    }

    m_pendingRequests.insert(requestId,
                             Daemon::ApiImpl::RequestProcessor::PendingRequest(
                                 callerPid,
                                 requestId,
                                 Daemon::ApiImpl::GetStandaloneSecretRequest,
                                 QVariantList() << QVariant::fromValue<Secret::Identifier>(identifier)
                                                << userInteractionMode
                                                << interactionServiceAddress
                                                << secretStoragePluginName
                                                << secretEncryptionPluginName
                                                << secretUnlockSemantic
                                                << secretCustomLockTimeoutMs));
    return Result(Result::Pending);
}

Result
Daemon::ApiImpl::RequestProcessor::getStandaloneSecretWithAuthenticationKey(
        pid_t callerPid,
        quint64 requestId,
        const Secret::Identifier &identifier,
        SecretManager::UserInteractionMode userInteractionMode,
        const QString &interactionServiceAddress,
        const QString &storagePluginName,
        const QString &encryptionPluginName,
        int secretUnlockSemantic,
        int secretCustomLockTimeoutMs,
        const QByteArray &authenticationKey,
        Secret *secret)
{
    // may be needed for access control requests in the future.
    Q_UNUSED(callerPid);
    Q_UNUSED(requestId);
    Q_UNUSED(userInteractionMode);
    Q_UNUSED(interactionServiceAddress);

    if (secretUnlockSemantic == SecretManager::CustomLockTimoutRelock) {
        if (!m_standaloneSecretLockTimers.contains(identifier.name())) {
            QTimer *timer = new QTimer(this);
            connect(timer, &QTimer::timeout,
                    this, &Daemon::ApiImpl::RequestProcessor::timeoutRelockSecret);
            timer->setInterval(secretCustomLockTimeoutMs);
            timer->setSingleShot(true);
            timer->start();
            m_standaloneSecretLockTimers.insert(identifier.name(), timer);
        }
    }

    const QString collectionName = QStringLiteral("standalone");
    const QString hashedSecretName = Daemon::Util::generateHashedSecretName(collectionName, identifier.name());

    Result pluginResult;
    if (storagePluginName == encryptionPluginName) {
        QString secretName;
        QByteArray secretData;
        Secret::FilterData secretFilterdata;
        pluginResult = m_encryptedStoragePlugins[storagePluginName]->accessSecret(collectionName, hashedSecretName, authenticationKey, &secretName, &secretData, &secretFilterdata);
        secret->setIdentifier(identifier);
        secret->setData(secretData);
        secret->setFilterData(secretFilterdata);
    } else {
        QByteArray encrypted, encryptedName;
        Secret::FilterData filterData;
        pluginResult = m_storagePlugins[storagePluginName]->getSecret(collectionName, hashedSecretName, &encryptedName, &encrypted, &filterData);
        if (pluginResult.code() == Result::Succeeded) {
            QByteArray decrypted;
            pluginResult = m_encryptionPlugins[encryptionPluginName]->decryptSecret(encrypted, authenticationKey, &decrypted);
            secret->setIdentifier(identifier);
            secret->setData(decrypted);
            secret->setFilterData(filterData);
        }
    }

    return pluginResult;
}

// find collection secrets via filter
Result
Daemon::ApiImpl::RequestProcessor::findCollectionSecrets(
        pid_t callerPid,
        quint64 requestId,
        const QString &collectionName,
        const Secret::FilterData &filter,
        SecretManager::FilterOperator filterOperator,
        SecretManager::UserInteractionMode userInteractionMode,
        const QString &interactionServiceAddress,
        QVector<Secret::Identifier> *identifiers)
{
    if (collectionName.isEmpty()) {
        return Result(Result::InvalidCollectionError,
                      QLatin1String("Empty collection name given"));
    } else if (collectionName.compare(QStringLiteral("standalone"), Qt::CaseInsensitive) == 0) {
        return Result(Result::InvalidCollectionError,
                      QLatin1String("Reserved collection name given"));
    } else if (filter.isEmpty()) {
        return Result(Result::InvalidFilterError,
                      QLatin1String("Empty filter given"));
    }

    // TODO: perform access control request to see if the application has permission to read secure storage data.
    const bool applicationIsPlatformApplication = m_appPermissions->applicationIsPlatformApplication(callerPid);
    const QString callerApplicationId = applicationIsPlatformApplication
                ? m_appPermissions->platformApplicationId()
                : m_appPermissions->applicationId(callerPid);

    bool found = false;
    QString collectionApplicationId;
    bool collectionUsesDeviceLockKey = false;
    QString collectionStoragePluginName;
    QString collectionEncryptionPluginName;
    QString collectionAuthenticationPluginName;
    int collectionUnlockSemantic = 0;
    int collectionCustomLockTimeoutMs = 0;
    SecretManager::AccessControlMode collectionAccessControlMode = SecretManager::OwnerOnlyMode;
    Result metadataResult = m_bkdb->collectionMetadata(
                collectionName,
                &found,
                &collectionApplicationId,
                &collectionUsesDeviceLockKey,
                &collectionStoragePluginName,
                &collectionEncryptionPluginName,
                &collectionAuthenticationPluginName,
                &collectionUnlockSemantic,
                &collectionCustomLockTimeoutMs,
                &collectionAccessControlMode);
    if (metadataResult.code() != Result::Succeeded) {
        return metadataResult;
    } else if (!found) {
        return Result(Result::InvalidCollectionError,
                      QLatin1String("Nonexistent collection name given"));
    }

    if (collectionStoragePluginName == collectionEncryptionPluginName && !m_encryptedStoragePlugins.contains(collectionStoragePluginName)) {
        // TODO: stale data, plugin was removed but data still exists...?
        return Result(Result::InvalidExtensionPluginError,
                      QString::fromLatin1("No such encrypted storage plugin exists: %1").arg(collectionStoragePluginName));
    } else if (collectionStoragePluginName != collectionEncryptionPluginName && !m_storagePlugins.contains(collectionStoragePluginName)) {
        // TODO: stale data, plugin was removed but data still exists...?
        return Result(Result::InvalidExtensionPluginError,
                      QString::fromLatin1("No such storage plugin exists: %1").arg(collectionStoragePluginName));
    } else if (collectionStoragePluginName != collectionEncryptionPluginName && !m_encryptionPlugins.contains(collectionEncryptionPluginName)) {
        // TODO: stale data, plugin was removed but data still exists...?
        return Result(Result::InvalidExtensionPluginError,
                      QString::fromLatin1("No such encryption plugin exists: %1").arg(collectionEncryptionPluginName));
    } else if (collectionAccessControlMode == SecretManager::SystemAccessControlMode) {
        // TODO: perform access control request, to ask for permission to set the secret in the collection.
        return Result(Result::OperationNotSupportedError,
                      QLatin1String("Access control requests are not currently supported. TODO!"));
    } else if (collectionAccessControlMode == SecretManager::OwnerOnlyMode
               && collectionApplicationId != callerApplicationId) {
        return Result(Result::PermissionsError,
                      QString::fromLatin1("Collection %1 is owned by a different application").arg(collectionName));
    } else if (!m_authenticationPlugins.contains(collectionAuthenticationPluginName)) {
        return Result(Result::InvalidExtensionPluginError,
                      QString::fromLatin1("No such authentication plugin available: %1").arg(collectionAuthenticationPluginName));
    }

    if (collectionStoragePluginName == collectionEncryptionPluginName) {
        bool locked = false;
        Result pluginResult = m_encryptedStoragePlugins[collectionStoragePluginName]->isLocked(collectionName, &locked);
        if (pluginResult.code() != Result::Succeeded) {
            return pluginResult;
        }

        if (locked) {
            if (collectionUsesDeviceLockKey) {
                return Result(Result::CollectionIsLockedError,
                              QString::fromLatin1("Collection %1 is locked and requires device lock authentication").arg(collectionName));
            } else {
                if (userInteractionMode == SecretManager::PreventInteraction) {
                    return Result(Result::OperationRequiresUserInteraction,
                                  QString::fromLatin1("Authentication plugin %1 requires user interaction").arg(collectionAuthenticationPluginName));
                } else if (m_authenticationPlugins[collectionAuthenticationPluginName]->authenticationTypes() & AuthenticationPlugin::ApplicationSpecificAuthentication
                            && (userInteractionMode != SecretManager::ApplicationInteraction || interactionServiceAddress.isEmpty())) {
                    return Result(Result::OperationRequiresApplicationUserInteraction,
                                  QString::fromLatin1("Authentication plugin %1 requires in-process user interaction").arg(collectionAuthenticationPluginName));
                }

                // perform the user input flow required to get the input key data which will be used
                // to decrypt the secret.
                InteractionParameters ikdRequest;
                ikdRequest.setApplicationId(callerApplicationId);
                ikdRequest.setCollectionName(collectionName);
                ikdRequest.setSecretName(QString());
                ikdRequest.setOperation(InteractionParameters::UnlockCollection);
                ikdRequest.setInputType(InteractionParameters::AlphaNumericInput);
                ikdRequest.setEchoMode(InteractionParameters::PasswordEchoOnEdit);
                ikdRequest.setPromptTrId("sailfish_secrets_unlock_collection_find_secrets_input_key_data_prompt");
                Result interactionResult = m_authenticationPlugins[collectionAuthenticationPluginName]->beginUserInputInteraction(
                            callerPid,
                            requestId,
                            ikdRequest,
                            interactionServiceAddress);
                if (interactionResult.code() == Result::Failed) {
                    return interactionResult;
                }

                m_pendingRequests.insert(requestId,
                                         Daemon::ApiImpl::RequestProcessor::PendingRequest(
                                             callerPid,
                                             requestId,
                                             Daemon::ApiImpl::FindCollectionSecretsRequest,
                                             QVariantList() << collectionName
                                                            << QVariant::fromValue<Secret::FilterData >(filter)
                                                            << filterOperator
                                                            << userInteractionMode
                                                            << interactionServiceAddress
                                                            << collectionStoragePluginName
                                                            << collectionEncryptionPluginName
                                                            << collectionUnlockSemantic
                                                            << collectionCustomLockTimeoutMs));
                return Result(Result::Pending);
            }
        } else {
            return findCollectionSecretsWithAuthenticationKey(
                        callerPid,
                        requestId,
                        collectionName,
                        filter,
                        filterOperator,
                        userInteractionMode,
                        interactionServiceAddress,
                        collectionStoragePluginName,
                        collectionEncryptionPluginName,
                        collectionUnlockSemantic,
                        collectionCustomLockTimeoutMs,
                        QByteArray(), // no key required, it's unlocked already.
                        identifiers);
        }
    } else {
        if (!m_collectionAuthenticationKeys.contains(collectionName)) {
            if (collectionUsesDeviceLockKey) {
                return Result(Result::CollectionIsLockedError,
                              QString::fromLatin1("Collection %1 is locked and requires device lock authentication").arg(collectionName));
            } else {
                if (userInteractionMode == SecretManager::PreventInteraction) {
                    return Result(Result::OperationRequiresUserInteraction,
                                  QString::fromLatin1("Authentication plugin %1 requires user interaction").arg(collectionAuthenticationPluginName));
                } else if (m_authenticationPlugins[collectionAuthenticationPluginName]->authenticationTypes() & AuthenticationPlugin::ApplicationSpecificAuthentication
                           && (userInteractionMode != SecretManager::ApplicationInteraction || interactionServiceAddress.isEmpty())) {
                    return Result(Result::OperationRequiresApplicationUserInteraction,
                                  QString::fromLatin1("Authentication plugin %1 requires in-process user interaction").arg(collectionAuthenticationPluginName));
                }

                // perform the user input flow required to get the input key data which will be used
                // to decrypt the secret.
                InteractionParameters ikdRequest;
                ikdRequest.setApplicationId(callerApplicationId);
                ikdRequest.setCollectionName(collectionName);
                ikdRequest.setSecretName(QString());
                ikdRequest.setOperation(InteractionParameters::UnlockCollection);
                ikdRequest.setInputType(InteractionParameters::AlphaNumericInput);
                ikdRequest.setEchoMode(InteractionParameters::PasswordEchoOnEdit);
                ikdRequest.setPromptTrId("sailfish_secrets_unlock_collection_find_secrets_input_key_data_prompt");
                Result interactionResult = m_authenticationPlugins[collectionAuthenticationPluginName]->beginUserInputInteraction(
                            callerPid,
                            requestId,
                            ikdRequest,
                            interactionServiceAddress);
                if (interactionResult.code() == Result::Failed) {
                    return interactionResult;
                }

                m_pendingRequests.insert(requestId,
                                         Daemon::ApiImpl::RequestProcessor::PendingRequest(
                                             callerPid,
                                             requestId,
                                             Daemon::ApiImpl::FindCollectionSecretsRequest,
                                             QVariantList() << collectionName
                                                            << QVariant::fromValue<Secret::FilterData >(filter)
                                                            << filterOperator
                                                            << userInteractionMode
                                                            << interactionServiceAddress
                                                            << collectionStoragePluginName
                                                            << collectionEncryptionPluginName
                                                            << collectionUnlockSemantic
                                                            << collectionCustomLockTimeoutMs));
                return Result(Result::Pending);
            }
        } else {
            return findCollectionSecretsWithAuthenticationKey(
                        callerPid,
                        requestId,
                        collectionName,
                        filter,
                        filterOperator,
                        userInteractionMode,
                        interactionServiceAddress,
                        collectionStoragePluginName,
                        collectionEncryptionPluginName,
                        collectionUnlockSemantic,
                        collectionCustomLockTimeoutMs,
                        m_collectionAuthenticationKeys.value(collectionName),
                        identifiers);
        }
    }
}

Result
Daemon::ApiImpl::RequestProcessor::findCollectionSecretsWithAuthenticationKey(
        pid_t callerPid,
        quint64 requestId,
        const QString &collectionName,
        const Secret::FilterData &filter,
        SecretManager::FilterOperator filterOperator,
        SecretManager::UserInteractionMode userInteractionMode,
        const QString &interactionServiceAddress,
        const QString &storagePluginName,
        const QString &encryptionPluginName,
        int collectionUnlockSemantic,
        int collectionCustomLockTimeoutMs,
        const QByteArray &authenticationKey,
        QVector<Secret::Identifier> *identifiers)
{
    // might be required in future for access control requests.
    Q_UNUSED(callerPid);
    Q_UNUSED(requestId);
    Q_UNUSED(userInteractionMode);
    Q_UNUSED(interactionServiceAddress);

    if (collectionUnlockSemantic == SecretManager::CustomLockTimoutRelock) {
        if (!m_collectionLockTimers.contains(collectionName)) {
            QTimer *timer = new QTimer(this);
            connect(timer, &QTimer::timeout,
                    this, &Daemon::ApiImpl::RequestProcessor::timeoutRelockCollection);
            timer->setInterval(collectionCustomLockTimeoutMs);
            timer->setSingleShot(true);
            timer->start();
            m_collectionLockTimers.insert(collectionName, timer);
        }
    }

    Result pluginResult;
    if (storagePluginName == encryptionPluginName) {
        bool locked = false;
        pluginResult = m_encryptedStoragePlugins[storagePluginName]->isLocked(collectionName, &locked);
        if (pluginResult.code() != Result::Succeeded) {
            return pluginResult;
        }
        // if it's locked, attempt to unlock it
        if (locked) {
            pluginResult = m_encryptedStoragePlugins[storagePluginName]->setEncryptionKey(collectionName, authenticationKey);
            if (pluginResult.code() != Result::Succeeded) {
                // unable to apply the new authenticationKey.
                m_encryptedStoragePlugins[storagePluginName]->setEncryptionKey(collectionName, QByteArray());
                return Result(Result::SecretsPluginDecryptionError,
                              QString::fromLatin1("Unable to decrypt collection %1 with the entered authentication key").arg(collectionName));

            }
            pluginResult = m_encryptedStoragePlugins[storagePluginName]->isLocked(collectionName, &locked);
            if (pluginResult.code() != Result::Succeeded) {
                m_encryptedStoragePlugins[storagePluginName]->setEncryptionKey(collectionName, QByteArray());
                return Result(Result::SecretsPluginDecryptionError,
                              QString::fromLatin1("Unable to check lock state of collection %1 after setting the entered authentication key").arg(collectionName));

            }
        }
        if (locked) {
            // still locked, even after applying the new authenticationKey?  The authenticationKey was wrong.
            m_encryptedStoragePlugins[storagePluginName]->setEncryptionKey(collectionName, QByteArray());
            return Result(Result::IncorrectAuthenticationKeyError,
                          QString::fromLatin1("The authentication key entered for collection %1 was incorrect").arg(collectionName));
        }
        // successfully unlocked the encrypted storage collection.  perform the filtering operation.
        pluginResult = m_encryptedStoragePlugins[storagePluginName]->findSecrets(collectionName, filter, static_cast<StoragePlugin::FilterOperator>(filterOperator), identifiers);
    } else {
        if (!m_collectionAuthenticationKeys.contains(collectionName)) {
            // TODO: some way to "test" the authenticationKey!  also, if it's a custom lock, set the timeout, etc.
            m_collectionAuthenticationKeys.insert(collectionName, authenticationKey);
        }

        QVector<QByteArray> encryptedSecretNames;
        pluginResult = m_storagePlugins[storagePluginName]->findSecrets(collectionName, filter, static_cast<StoragePlugin::FilterOperator>(filterOperator), &encryptedSecretNames);
        if (pluginResult.code() == Result::Succeeded) {
            // decrypt each of the secret names.
            QVector<QString> decryptedSecretNames;
            bool decryptionSucceeded = true;
            for (const QByteArray &esn : encryptedSecretNames) {
                QByteArray decryptedName;
                pluginResult = m_encryptionPlugins[encryptionPluginName]->decryptSecret(esn, m_collectionAuthenticationKeys.value(collectionName), &decryptedName);
                if (pluginResult.code() != Result::Succeeded) {
                    decryptionSucceeded = false;
                    break;
                }
                decryptedSecretNames.append(QString::fromUtf8(decryptedName));
            }
            if (decryptionSucceeded) {
                for (const QString &secretName : decryptedSecretNames) {
                    identifiers->append(Secret::Identifier(secretName, collectionName));
                }
            }
        }
    }

    return pluginResult;
}

// find standalone secrets via filter
Result
Daemon::ApiImpl::RequestProcessor::findStandaloneSecrets(
        pid_t callerPid,
        quint64 requestId,
        const Secret::FilterData &filter,
        SecretManager::FilterOperator filterOperator,
        SecretManager::UserInteractionMode userInteractionMode,
        const QString &interactionServiceAddress,
        QVector<Secret::Identifier> *identifiers)
{
    // TODO!
    Q_UNUSED(callerPid)
    Q_UNUSED(requestId)
    Q_UNUSED(filter)
    Q_UNUSED(filterOperator)
    Q_UNUSED(userInteractionMode)
    Q_UNUSED(interactionServiceAddress)
    Q_UNUSED(identifiers)
    return Result(Result::OperationNotSupportedError,
                  QLatin1String("Filtering standalone secrets is not yet supported!"));
}

// delete a secret in a collection
Result
Daemon::ApiImpl::RequestProcessor::deleteCollectionSecret(
        pid_t callerPid,
        quint64 requestId,
        const Secret::Identifier &identifier,
        SecretManager::UserInteractionMode userInteractionMode,
        const QString &interactionServiceAddress)
{
    if (identifier.name().isEmpty()) {
        return Result(Result::InvalidSecretError,
                      QLatin1String("Empty secret name given"));
    } else if (identifier.collectionName().isEmpty()) {
        return Result(Result::InvalidCollectionError,
                      QLatin1String("Empty collection name given"));
    } else if (identifier.collectionName().compare(QStringLiteral("standalone"), Qt::CaseInsensitive) == 0) {
        return Result(Result::InvalidCollectionError,
                      QLatin1String("Reserved collection name given"));
    }

    // TODO: perform access control request to see if the application has permission to write secure storage data.
    const bool applicationIsPlatformApplication = m_appPermissions->applicationIsPlatformApplication(callerPid);
    const QString callerApplicationId = applicationIsPlatformApplication
                ? m_appPermissions->platformApplicationId()
                : m_appPermissions->applicationId(callerPid);

    bool found = false;
    QString collectionApplicationId;
    bool collectionUsesDeviceLockKey = false;
    QString collectionStoragePluginName;
    QString collectionEncryptionPluginName;
    QString collectionAuthenticationPluginName;
    SecretManager::AccessControlMode collectionAccessControlMode = SecretManager::OwnerOnlyMode;
    Result metadataResult = m_bkdb->collectionMetadata(
                identifier.collectionName(),
                &found,
                &collectionApplicationId,
                &collectionUsesDeviceLockKey,
                &collectionStoragePluginName,
                &collectionEncryptionPluginName,
                &collectionAuthenticationPluginName,
                Q_NULLPTR,
                Q_NULLPTR,
                &collectionAccessControlMode);
    if (metadataResult.code() != Result::Succeeded) {
        return metadataResult;
    } else if (!found) {
        return Result(Result::InvalidCollectionError,
                      QLatin1String("Nonexistent collection name given"));
    }

    if (collectionAccessControlMode == SecretManager::SystemAccessControlMode) {
        // TODO: perform access control request, to ask for permission to set the secret in the collection.
        return Result(Result::OperationNotSupportedError,
                      QLatin1String("Access control requests are not currently supported. TODO!"));
    } else if (collectionAccessControlMode == SecretManager::OwnerOnlyMode
               && collectionApplicationId != callerApplicationId) {
        return Result(Result::PermissionsError,
                      QString::fromLatin1("Collection %1 is owned by a different application").arg(identifier.collectionName()));
    } else if (collectionStoragePluginName == collectionEncryptionPluginName
            && !m_encryptedStoragePlugins.contains(collectionStoragePluginName)) {
        // TODO: this means we have "stale" data in the database; what should we do in this case?
        return Result(Result::InvalidExtensionPluginError,
                      QString::fromLatin1("No such encrypted storage plugin exists: %1").arg(collectionStoragePluginName));
    } else if (collectionStoragePluginName != collectionEncryptionPluginName
            && (collectionStoragePluginName.isEmpty() || !m_storagePlugins.contains(collectionStoragePluginName))) {
        // TODO: this means we have "stale" data in the database; what should we do in this case?
        return Result(Result::InvalidExtensionPluginError,
                      QString::fromLatin1("No such storage plugin exists: %1").arg(collectionStoragePluginName));
    } else if (collectionStoragePluginName != collectionEncryptionPluginName
            && (collectionEncryptionPluginName.isEmpty() || !m_encryptionPlugins.contains(collectionEncryptionPluginName))) {
        // TODO: this means we have "stale" data in the database; what should we do in this case?
        return Result(Result::InvalidExtensionPluginError,
                      QString::fromLatin1("No such encryption plugin exists: %1").arg(collectionEncryptionPluginName));
    }

    if (collectionStoragePluginName == collectionEncryptionPluginName) {
        bool locked = false;
        Result pluginResult = m_encryptedStoragePlugins[collectionStoragePluginName]->isLocked(identifier.collectionName(), &locked);
        if (pluginResult.code() != Result::Succeeded) {
            return pluginResult;
        }
        if (locked) {
            if (collectionUsesDeviceLockKey) {
                return Result(Result::CollectionIsLockedError,
                              QString::fromLatin1("Collection %1 is locked and requires device lock authentication").arg(identifier.collectionName()));
            }

            if (userInteractionMode == SecretManager::PreventInteraction) {
                return Result(Result::OperationRequiresUserInteraction,
                              QString::fromLatin1("Authentication plugin %1 requires user interaction").arg(collectionAuthenticationPluginName));
            }

            // perform the user input flow required to get the input key data which will be used
            // to unlock the secret for deletion.
            InteractionParameters ikdRequest;
            ikdRequest.setApplicationId(callerApplicationId);
            ikdRequest.setCollectionName(identifier.collectionName());
            ikdRequest.setSecretName(identifier.name());
            ikdRequest.setOperation(InteractionParameters::DeleteSecret);
            ikdRequest.setInputType(InteractionParameters::AlphaNumericInput);
            ikdRequest.setEchoMode(InteractionParameters::PasswordEchoOnEdit);
            ikdRequest.setPromptTrId("sailfish_secrets_delete_collection_secret_input_key_data_prompt");
            Result interactionResult = m_authenticationPlugins[collectionAuthenticationPluginName]->beginUserInputInteraction(
                        callerPid,
                        requestId,
                        ikdRequest,
                        interactionServiceAddress);
            if (interactionResult.code() == Result::Failed) {
                return interactionResult;
            }

            m_pendingRequests.insert(requestId,
                                     Daemon::ApiImpl::RequestProcessor::PendingRequest(
                                         callerPid,
                                         requestId,
                                         Daemon::ApiImpl::DeleteCollectionSecretRequest,
                                         QVariantList() << QVariant::fromValue<Secret::Identifier>(identifier)
                                                        << userInteractionMode
                                                        << interactionServiceAddress));
            return Result(Result::Pending);
        } else {
            return deleteCollectionSecretWithAuthenticationKey(
                        callerPid,
                        requestId,
                        identifier,
                        userInteractionMode,
                        interactionServiceAddress,
                        DeviceLockKey);
        }
    } else {
        if (!m_collectionAuthenticationKeys.contains(identifier.collectionName())) {
            if (collectionUsesDeviceLockKey) {
                return Result(Result::CollectionIsLockedError,
                                                 QStringLiteral("Collection %1 is locked and requires device lock authentication").arg(identifier.collectionName()));
            } else {
                if (userInteractionMode == SecretManager::PreventInteraction) {
                    return Result(Result::OperationRequiresUserInteraction,
                                  QString::fromLatin1("Authentication plugin %1 requires user interaction").arg(collectionAuthenticationPluginName));
                }

                // perform the user input flow required to get the input key data which will be used
                // to unlock the secret for deletion.
                InteractionParameters ikdRequest;
                ikdRequest.setApplicationId(callerApplicationId);
                ikdRequest.setCollectionName(identifier.collectionName());
                ikdRequest.setSecretName(identifier.name());
                ikdRequest.setOperation(InteractionParameters::DeleteSecret);
                ikdRequest.setInputType(InteractionParameters::AlphaNumericInput);
                ikdRequest.setEchoMode(InteractionParameters::PasswordEchoOnEdit);
                ikdRequest.setPromptTrId("sailfish_secrets_delete_collection_secret_input_key_data_prompt");
                Result interactionResult = m_authenticationPlugins[collectionAuthenticationPluginName]->beginUserInputInteraction(
                            callerPid,
                            requestId,
                            ikdRequest,
                            interactionServiceAddress);
                if (interactionResult.code() == Result::Failed) {
                    return interactionResult;
                }

                m_pendingRequests.insert(requestId,
                                         Daemon::ApiImpl::RequestProcessor::PendingRequest(
                                             callerPid,
                                             requestId,
                                             Daemon::ApiImpl::DeleteCollectionSecretRequest,
                                             QVariantList() << QVariant::fromValue<Secret::Identifier>(identifier)
                                                            << userInteractionMode
                                                            << interactionServiceAddress));
                return Result(Result::Pending);
            }
        } else {
            return deleteCollectionSecretWithAuthenticationKey(
                        callerPid,
                        requestId,
                        identifier,
                        userInteractionMode,
                        interactionServiceAddress,
                        m_collectionAuthenticationKeys.value(identifier.collectionName()));
        }
    }
}

Result
Daemon::ApiImpl::RequestProcessor::deleteCollectionSecretWithAuthenticationKey(
        pid_t callerPid,
        quint64 requestId,
        const Secret::Identifier &identifier,
        SecretManager::UserInteractionMode userInteractionMode,
        const QString &interactionServiceAddress,
        const QByteArray &authenticationKey)
{
    // may be needed for access control requests in the future.
    Q_UNUSED(requestId);
    Q_UNUSED(userInteractionMode);
    Q_UNUSED(interactionServiceAddress);

    // TODO: perform access control request to see if the application has permission to write secure storage data.
    const bool applicationIsPlatformApplication = m_appPermissions->applicationIsPlatformApplication(callerPid);
    const QString callerApplicationId = applicationIsPlatformApplication
                ? m_appPermissions->platformApplicationId()
                : m_appPermissions->applicationId(callerPid);

    // check again in case it was deleted or modified while the
    // asynchronous authentication key request was in progress.
    bool found = false;
    QString collectionApplicationId;
    bool collectionUsesDeviceLockKey = false;
    QString collectionStoragePluginName;
    QString collectionEncryptionPluginName;
    QString collectionAuthenticationPluginName;
    SecretManager::AccessControlMode collectionAccessControlMode = SecretManager::OwnerOnlyMode;
    Result metadataResult = m_bkdb->collectionMetadata(
                identifier.collectionName(),
                &found,
                &collectionApplicationId,
                &collectionUsesDeviceLockKey,
                &collectionStoragePluginName,
                &collectionEncryptionPluginName,
                &collectionAuthenticationPluginName,
                Q_NULLPTR,
                Q_NULLPTR,
                &collectionAccessControlMode);
    if (metadataResult.code() != Result::Succeeded) {
        return metadataResult;
    } else if (!found) {
        return Result(Result::InvalidCollectionError,
                      QLatin1String("Nonexistent collection name given"));
    }

    if (collectionUsesDeviceLockKey && authenticationKey != DeviceLockKey) {
        return Result(Result::IncorrectAuthenticationKeyError,
                      QLatin1String("Incorrect device lock key provided"));
    }

    if (collectionAccessControlMode == SecretManager::SystemAccessControlMode) {
        // TODO: perform access control request, to ask for permission to set the secret in the collection.
        return Result(Result::OperationNotSupportedError,
                      QLatin1String("Access control requests are not currently supported. TODO!"));
    } else if (collectionAccessControlMode == SecretManager::OwnerOnlyMode
               && collectionApplicationId != callerApplicationId) {
        return Result(Result::PermissionsError,
                      QString::fromLatin1("Collection %1 is owned by a different application").arg(identifier.collectionName()));
    }

    const QString hashedSecretName = Daemon::Util::generateHashedSecretName(identifier.collectionName(), identifier.name());
    Result pluginResult;
    if (collectionStoragePluginName == collectionEncryptionPluginName) {
        bool locked = false;
        pluginResult = m_encryptedStoragePlugins[collectionStoragePluginName]->isLocked(identifier.collectionName(), &locked);
        if (pluginResult.code() != Result::Succeeded) {
            return pluginResult;
        }
        // if it's locked, attempt to unlock it
        if (locked) {
            pluginResult = m_encryptedStoragePlugins[collectionStoragePluginName]->setEncryptionKey(identifier.collectionName(), authenticationKey);
            if (pluginResult.code() != Result::Succeeded) {
                // unable to apply the new authenticationKey.
                m_encryptedStoragePlugins[collectionStoragePluginName]->setEncryptionKey(identifier.collectionName(), QByteArray());
                return Result(Result::SecretsPluginDecryptionError,
                              QString::fromLatin1("Unable to decrypt collection %1 with the entered authentication key").arg(identifier.collectionName()));

            }
            pluginResult = m_encryptedStoragePlugins[collectionStoragePluginName]->isLocked(identifier.collectionName(), &locked);
            if (pluginResult.code() != Result::Succeeded) {
                m_encryptedStoragePlugins[collectionStoragePluginName]->setEncryptionKey(identifier.collectionName(), QByteArray());
                return Result(Result::SecretsPluginDecryptionError,
                              QString::fromLatin1("Unable to check lock state of collection %1 after setting the entered authentication key").arg(identifier.collectionName()));

            }
        }
        if (locked) {
            // still locked, even after applying the new authenticationKey?  The authenticationKey was wrong.
            m_encryptedStoragePlugins[collectionStoragePluginName]->setEncryptionKey(identifier.collectionName(), QByteArray());
            return Result(Result::IncorrectAuthenticationKeyError,
                          QString::fromLatin1("The authentication key entered for collection %1 was incorrect").arg(identifier.collectionName()));
        }
        // successfully unlocked the encrypted storage collection.  remove the secret.
        pluginResult = m_encryptedStoragePlugins[collectionStoragePluginName]->removeSecret(identifier.collectionName(), hashedSecretName);
    } else {
        if (!m_collectionAuthenticationKeys.contains(identifier.collectionName())) {
            // TODO: some way to "test" the authenticationKey!  also, if it's a custom lock, set the timeout, etc.
            m_collectionAuthenticationKeys.insert(identifier.collectionName(), authenticationKey);
        }

        pluginResult = m_storagePlugins[collectionStoragePluginName]->removeSecret(identifier.collectionName(), hashedSecretName);
    }

    // now remove from the master database.
    if (pluginResult.code() == Result::Succeeded) {
        Result deleteResult = m_bkdb->deleteSecret(identifier.collectionName(),
                                                   hashedSecretName);
        if (deleteResult.code() != Result::Succeeded) {
            // TODO: add a "dirty" flag for this secret somewhere in memory, so we can try again later.
            return deleteResult; // once the dirty flag has been added, don't return this error but just continue.
        }
    }

    if (pluginResult.code() == Result::Succeeded
            && collectionAccessControlMode == SecretManager::SystemAccessControlMode) {
        // TODO: tell AccessControl daemon to remove this datum from its database.
    }

    return pluginResult;
}

// delete a standalone secret
Result
Daemon::ApiImpl::RequestProcessor::deleteStandaloneSecret(
        pid_t callerPid,
        quint64 requestId,
        const Secret::Identifier &identifier,
        SecretManager::UserInteractionMode userInteractionMode)
{
    // these may be required in the future for access control requests.
    Q_UNUSED(requestId);
    Q_UNUSED(userInteractionMode);

    // TODO: perform access control request to see if the application has permission to write secure storage data.
    const bool applicationIsPlatformApplication = m_appPermissions->applicationIsPlatformApplication(callerPid);
    const QString callerApplicationId = applicationIsPlatformApplication
                ? m_appPermissions->platformApplicationId()
                : m_appPermissions->applicationId(callerPid);

    bool found = false;
    QString secretApplicationId;
    bool secretUsesDeviceLockKey = false;
    QString secretStoragePluginName;
    QString secretEncryptionPluginName;
    SecretManager::AccessControlMode secretAccessControlMode = SecretManager::OwnerOnlyMode;
    const QString collectionName = QStringLiteral("standalone");
    const QString hashedSecretName = Daemon::Util::generateHashedSecretName(collectionName, identifier.name());
    Result metadataResult = m_bkdb->secretMetadata(
                collectionName,
                hashedSecretName,
                &found,
                &secretApplicationId,
                &secretUsesDeviceLockKey,
                &secretStoragePluginName,
                &secretEncryptionPluginName,
                Q_NULLPTR,
                Q_NULLPTR,
                Q_NULLPTR,
                &secretAccessControlMode);
    if (metadataResult.code() != Result::Succeeded) {
        return metadataResult;
    } else if (!found) {
        // the secret doesn't exist, return success.
        return Result(Result::Succeeded);
    }

    if (secretAccessControlMode == SecretManager::SystemAccessControlMode) {
        // TODO: perform access control request, to ask for permission to set the secret in the collection.
        return Result(Result::OperationNotSupportedError,
                      QLatin1String("Access control requests are not currently supported. TODO!"));
    } else if (secretAccessControlMode == SecretManager::OwnerOnlyMode
               && secretApplicationId != callerApplicationId) {
        return Result(Result::PermissionsError,
                      QString::fromLatin1("Secret %1 is owned by a different application").arg(identifier.name()));
    } else if (secretStoragePluginName == secretEncryptionPluginName && !m_encryptedStoragePlugins.contains(secretStoragePluginName)) {
        return Result(Result::InvalidExtensionPluginError,
                      QString::fromLatin1("No such encrypted storage plugin exists: %1").arg(secretStoragePluginName));
    } else if (secretStoragePluginName != secretEncryptionPluginName && !m_storagePlugins.contains(secretStoragePluginName)) {
        return Result(Result::InvalidExtensionPluginError,
                      QString::fromLatin1("No such storage plugin exists: %1").arg(secretStoragePluginName));
    } else if (secretStoragePluginName != secretEncryptionPluginName && !m_encryptionPlugins.contains(secretEncryptionPluginName)) {
        return Result(Result::InvalidExtensionPluginError,
                      QString::fromLatin1("No such encryption plugin exists: %1").arg(secretEncryptionPluginName));
    }

    Result pluginResult;
    if (secretStoragePluginName == secretEncryptionPluginName) {
        bool locked = false;
        pluginResult = m_encryptedStoragePlugins[secretStoragePluginName]->isLocked(collectionName, &locked);
        if (pluginResult.code() == Result::Failed) {
            return pluginResult;
        }
        if (locked && secretUsesDeviceLockKey) {
            pluginResult = m_encryptedStoragePlugins[secretStoragePluginName]->setEncryptionKey(collectionName, DeviceLockKey);
            if (pluginResult.code() == Result::Failed) {
                return pluginResult;
            }
        }
        pluginResult = m_encryptedStoragePlugins[secretStoragePluginName]->removeSecret(collectionName, hashedSecretName);
        if (locked) {
            // relock after delete-access.
            m_encryptedStoragePlugins[secretStoragePluginName]->setEncryptionKey(collectionName, QByteArray());
        }
    } else {
        pluginResult = m_storagePlugins[secretStoragePluginName]->removeSecret(collectionName, hashedSecretName);
        if (pluginResult.code() == Result::Succeeded) {
            m_standaloneSecretAuthenticationKeys.remove(hashedSecretName);
            m_standaloneSecretLockTimers.remove(hashedSecretName);
        }
    }

    // remove from master database also.
    if (pluginResult.code() == Result::Succeeded) {
        Result deleteResult = m_bkdb->deleteSecret(collectionName, hashedSecretName);
        if (deleteResult.code() != Result::Succeeded) {
            // TODO: add a "dirty" flag for this secret somewhere in memory, so we can try again later.
            return deleteResult; // once the dirty flag has been added, don't return error here but continue.
        }
    }

    return pluginResult;
}

void
Daemon::ApiImpl::RequestProcessor::userInputInteractionCompleted(
        uint callerPid,
        qint64 requestId,
        const InteractionParameters &interactionParameters,
        const QString &interactionServiceAddress,
        const Result &result,
        const QByteArray &userInput)
{
    // may be needed in the future for "multiple-step" flows.
    Q_UNUSED(callerPid);
    Q_UNUSED(interactionParameters)
    Q_UNUSED(interactionServiceAddress);

    bool returnUserInput = false;
    Secret secret;
    QVector<Secret::Identifier> identifiers;
    Result returnResult = result;
    if (result.code() == Result::Succeeded) {
        // look up the pending request in our list
        if (m_pendingRequests.contains(requestId)) {
            // call the appropriate method to complete the request
            Daemon::ApiImpl::RequestProcessor::PendingRequest pr = m_pendingRequests.take(requestId);
            switch (pr.requestType) {
                case CreateCustomLockCollectionRequest: {
                    if (pr.parameters.size() != 9) {
                        returnResult = Result(Result::UnknownError,
                                              QLatin1String("Internal error: incorrect parameter count!"));
                    } else {
                        returnResult = createCustomLockCollectionWithAuthenticationKey(
                                    pr.callerPid,
                                    pr.requestId,
                                    pr.parameters.takeFirst().value<QString>(),
                                    pr.parameters.takeFirst().value<QString>(),
                                    pr.parameters.takeFirst().value<QString>(),
                                    pr.parameters.takeFirst().value<QString>(),
                                    static_cast<SecretManager::CustomLockUnlockSemantic>(pr.parameters.takeFirst().value<int>()),
                                    pr.parameters.takeFirst().value<int>(),
                                    static_cast<SecretManager::AccessControlMode>(pr.parameters.takeFirst().value<int>()),
                                    static_cast<SecretManager::UserInteractionMode>(pr.parameters.takeFirst().value<int>()),
                                    pr.parameters.takeFirst().value<QString>(),
                                    userInput);
                    }
                    break;
                }
                case SetCollectionUserInputSecretRequest: {
                    if (pr.parameters.size() != 12) {
                        returnResult = Result(Result::UnknownError,
                                              QLatin1String("Internal error: incorrect parameter count!"));
                    } else {
                        Secret secret = pr.parameters.takeFirst().value<Secret>();
                        secret.setData(userInput);
                        /*InteractionParameters uiParams = */pr.parameters.takeFirst().value<InteractionParameters>();
                        returnResult = setCollectionSecretGetAuthenticationKey(
                                    pr.callerPid,
                                    pr.requestId,
                                    secret,
                                    static_cast<SecretManager::UserInteractionMode>(pr.parameters.takeFirst().value<int>()),
                                    pr.parameters.takeFirst().value<QString>(),
                                    pr.parameters.takeFirst().value<bool>(),
                                    pr.parameters.takeFirst().value<QString>(),
                                    pr.parameters.takeFirst().value<QString>(),
                                    pr.parameters.takeFirst().value<QString>(),
                                    pr.parameters.takeFirst().value<QString>(),
                                    pr.parameters.takeFirst().value<int>(),
                                    pr.parameters.takeFirst().value<int>(),
                                    static_cast<SecretManager::AccessControlMode>(pr.parameters.takeFirst().value<int>()));
                    }
                    break;
                }
                case SetCollectionSecretRequest: {
                    if (pr.parameters.size() != 12) {
                        returnResult = Result(Result::UnknownError,
                                              QLatin1String("Internal error: incorrect parameter count!"));
                    } else {
                        returnResult = setCollectionSecretWithAuthenticationKey(
                                    pr.callerPid,
                                    pr.requestId,
                                    pr.parameters.takeFirst().value<Secret>(),
                                    static_cast<SecretManager::UserInteractionMode>(pr.parameters.takeFirst().value<int>()),
                                    pr.parameters.takeFirst().value<QString>(),
                                    pr.parameters.takeFirst().value<bool>(),
                                    pr.parameters.takeFirst().value<QString>(),
                                    pr.parameters.takeFirst().value<QString>(),
                                    pr.parameters.takeFirst().value<QString>(),
                                    pr.parameters.takeFirst().value<QString>(),
                                    pr.parameters.takeFirst().value<int>(),
                                    pr.parameters.takeFirst().value<int>(),
                                    static_cast<SecretManager::AccessControlMode>(pr.parameters.takeFirst().value<int>()),
                                    userInput);
                    }
                    break;
                }
                case SetStandaloneDeviceLockUserInputSecretRequest: {
                    if (pr.parameters.size() != 9) {
                        returnResult = Result(Result::UnknownError,
                                              QLatin1String("Internal error: incorrect parameter count!"));
                    } else {
                        Secret secret = pr.parameters.takeFirst().value<Secret>();
                        secret.setData(userInput);
                        returnResult = writeStandaloneDeviceLockSecret(
                                    pr.callerPid,
                                    pr.requestId,
                                    pr.parameters.takeFirst().value<QString>(),
                                    pr.parameters.takeFirst().value<QString>(),
                                    pr.parameters.takeFirst().value<QString>(),
                                    secret,
                                    pr.parameters.takeFirst().value<QString>(),
                                    pr.parameters.takeFirst().value<QString>(),
                                    pr.parameters.takeFirst().value<bool>(),
                                    static_cast<SecretManager::DeviceLockUnlockSemantic>(pr.parameters.takeFirst().value<int>()),
                                    static_cast<SecretManager::AccessControlMode>(pr.parameters.takeFirst().value<int>()));
                    }
                    break;
                }
                case SetStandaloneCustomLockUserInputSecretRequest: {
                    if (pr.parameters.size() != 10) {
                        returnResult = Result(Result::UnknownError,
                                              QLatin1String("Internal error: incorrect parameter count!"));
                    } else {
                        Secret secret = pr.parameters.takeFirst().value<Secret>();
                        secret.setData(userInput);
                        returnResult = setStandaloneCustomLockSecretGetAuthenticationKey(
                                    pr.callerPid,
                                    pr.requestId,
                                    pr.parameters.takeFirst().value<QString>(),
                                    pr.parameters.takeFirst().value<QString>(),
                                    pr.parameters.takeFirst().value<QString>(),
                                    pr.parameters.takeFirst().value<QString>(),
                                    secret,
                                    static_cast<SecretManager::CustomLockUnlockSemantic>(pr.parameters.takeFirst().value<int>()),
                                    pr.parameters.takeFirst().value<int>(),
                                    static_cast<SecretManager::AccessControlMode>(pr.parameters.takeFirst().value<int>()),
                                    static_cast<SecretManager::UserInteractionMode>(pr.parameters.takeFirst().value<int>()),
                                    pr.parameters.takeFirst().value<QString>());
                    }
                    break;
                }
                case SetStandaloneCustomLockSecretRequest: {
                    if (pr.parameters.size() != 9) {
                        returnResult = Result(Result::UnknownError,
                                              QLatin1String("Internal error: incorrect parameter count!"));
                    } else {
                        returnResult = setStandaloneCustomLockSecretWithAuthenticationKey(
                                    pr.callerPid,
                                    pr.requestId,
                                    pr.parameters.takeFirst().value<QString>(),
                                    pr.parameters.takeFirst().value<QString>(),
                                    pr.parameters.takeFirst().value<QString>(),
                                    pr.parameters.takeFirst().value<Secret>(),
                                    static_cast<SecretManager::CustomLockUnlockSemantic>(pr.parameters.takeFirst().value<int>()),
                                    pr.parameters.takeFirst().value<int>(),
                                    static_cast<SecretManager::AccessControlMode>(pr.parameters.takeFirst().value<int>()),
                                    static_cast<SecretManager::UserInteractionMode>(pr.parameters.takeFirst().value<int>()),
                                    pr.parameters.takeFirst().value<QString>(),
                                    userInput);
                    }
                    break;
                }
                case GetCollectionSecretRequest: {
                    if (pr.parameters.size() != 7) {
                        returnResult = Result(Result::UnknownError,
                                              QLatin1String("Internal error: incorrect parameter count!"));
                    } else {
                        returnResult = getCollectionSecretWithAuthenticationKey(
                                    pr.callerPid,
                                    pr.requestId,
                                    pr.parameters.takeFirst().value<Secret::Identifier>(),
                                    static_cast<SecretManager::UserInteractionMode>(pr.parameters.takeFirst().value<int>()),
                                    pr.parameters.takeFirst().value<QString>(),
                                    pr.parameters.takeFirst().value<QString>(),
                                    pr.parameters.takeFirst().value<QString>(),
                                    pr.parameters.takeFirst().value<int>(),
                                    pr.parameters.takeFirst().value<int>(),
                                    userInput,
                                    &secret);
                    }
                    break;
                }
                case GetStandaloneSecretRequest: {
                    if (pr.parameters.size() != 7) {
                        returnResult = Result(Result::UnknownError,
                                              QLatin1String("Internal error: incorrect parameter count!"));
                    } else {
                        returnResult = getStandaloneSecretWithAuthenticationKey(
                                    pr.callerPid,
                                    pr.requestId,
                                    pr.parameters.takeFirst().value<Secret::Identifier>(),
                                    static_cast<SecretManager::UserInteractionMode>(pr.parameters.takeFirst().value<int>()),
                                    pr.parameters.takeFirst().value<QString>(),
                                    pr.parameters.takeFirst().value<QString>(),
                                    pr.parameters.takeFirst().value<QString>(),
                                    pr.parameters.takeFirst().value<int>(),
                                    pr.parameters.takeFirst().value<int>(),
                                    userInput,
                                    &secret);
                    }
                    break;
                }
                case FindCollectionSecretsRequest: {
                    if (pr.parameters.size() != 9) {
                        returnResult = Result(Result::UnknownError,
                                              QLatin1String("Internal error: incorrect parameter count!"));
                    } else {
                        returnResult = findCollectionSecretsWithAuthenticationKey(
                                    pr.callerPid,
                                    pr.requestId,
                                    pr.parameters.takeFirst().value<QString>(),
                                    pr.parameters.takeFirst().value<Secret::FilterData >(),
                                    static_cast<SecretManager::FilterOperator>(pr.parameters.takeFirst().value<int>()),
                                    static_cast<SecretManager::UserInteractionMode>(pr.parameters.takeFirst().value<int>()),
                                    pr.parameters.takeFirst().value<QString>(),
                                    pr.parameters.takeFirst().value<QString>(),
                                    pr.parameters.takeFirst().value<QString>(),
                                    pr.parameters.takeFirst().value<int>(),
                                    pr.parameters.takeFirst().value<int>(),
                                    userInput,
                                    &identifiers);
                    }
                    break;
                }
                case DeleteCollectionSecretRequest: {
                    if (pr.parameters.size() != 3) {
                        returnResult = Result(Result::UnknownError,
                                              QLatin1String("Internal error: incorrect parameter count!"));
                    } else {
                        returnResult = deleteCollectionSecretWithAuthenticationKey(
                                    pr.callerPid,
                                    pr.requestId,
                                    pr.parameters.takeFirst().value<Secret::Identifier>(),
                                    static_cast<SecretManager::UserInteractionMode>(pr.parameters.takeFirst().value<int>()),
                                    pr.parameters.takeFirst().value<QString>(),
                                    userInput);
                    }
                    break;
                }
                case UserInputRequest: {
                    if (pr.parameters.size() != 1) {
                        returnResult = Result(Result::UnknownError,
                                              QLatin1String("Internal error: incorrect parameter count!"));
                    } else {
                        returnUserInput = true;
                        returnResult = result; // Succeeded.
                    }
                    break;
                }
                default: {
                    returnResult = Result(Result::UnknownError,
                                          QLatin1String("Internal error: unknown continuation for asynchronous request!"));
                    break;
                }
            }
        } else {
            returnResult = Result(Result::UnknownError,
                                  QLatin1String("Internal error: failed to finish unknown pending request!"));
        }
    }

    // finish the request unless another asynchronous request is required.
    if (returnResult.code() != Result::Pending) {
        QList<QVariant> outParams;
        outParams << QVariant::fromValue<Result>(returnResult);
        if (secret.identifier().isValid()) {
            outParams << QVariant::fromValue<Secret>(secret);
        } else if (returnUserInput) {
            outParams << QVariant::fromValue<QByteArray>(userInput);
        } else {
            outParams << QVariant::fromValue<QVector<Secret::Identifier> >(identifiers);
        }
        m_requestQueue->requestFinished(requestId, outParams);
    }
}

void Daemon::ApiImpl::RequestProcessor::authenticationCompleted(
        uint callerPid,
        qint64 requestId,
        const Result &result)
{
    Q_UNUSED(callerPid);
    Q_UNUSED(requestId);
    Q_UNUSED(result);

    // the user has successfully authenticated themself.
    // in the future, use this to unlock master-locked collections.
}

void Daemon::ApiImpl::RequestProcessor::timeoutRelockCollection()
{
    QTimer *timer = qobject_cast<QTimer*>(sender());
    for (QMap<QString, QTimer*>::iterator it = m_collectionLockTimers.begin(); it != m_collectionLockTimers.end(); it++) {
        if (it.value() == timer) {
            qCDebug(lcSailfishSecretsDaemon) << "Relocking collection:" << it.key() << "due to unlock timeout!";
            m_collectionAuthenticationKeys.remove(it.key());
            m_collectionLockTimers.erase(it);
            break;
        }
    }
    timer->deleteLater();
}

void Daemon::ApiImpl::RequestProcessor::timeoutRelockSecret()
{
    QTimer *timer = qobject_cast<QTimer*>(sender());
    for (QMap<QString, QTimer*>::iterator it = m_standaloneSecretLockTimers.begin(); it != m_standaloneSecretLockTimers.end(); it++) {
        if (it.value() == timer) {
            qCDebug(lcSailfishSecretsDaemon) << "Relocking standalone secret:" << it.key() << "due to unlock timeout!";
            m_standaloneSecretAuthenticationKeys.remove(it.key());
            m_standaloneSecretLockTimers.erase(it);
            break;
        }
    }
    timer->deleteLater();
}

