# SGRN Encrypted Storage Architecture & Key Management

> [!WARNING]
> **CRITICAL SECURITY DISCLAIMER: DEMO CONFIGURATION**
> In the current implementation, encryption keys (DEKs) are stored locally in an SQLite database (`$SGRN_DATA_DIR/keys/keys.db`) with permissions `0600` under a directory with mode `0700`.
>
> **The SGRN server process and anyone with root or filesystem access to the host machine can read the encryption keys.**
>
> Platform administrators are **NOT** cryptographically separated from domain data under this configuration. This implementation provides encryption-at-rest against unauthorized storage drive/bucket access (e.g. offline theft of raw S3/Garage storage media), but does **NOT** provide cryptographic zero-trust separation between platform operators and tenant domain data.

---

## 1. Threat Model & Guarantees

### What is Protected

- **Storage Media Exposure**: A leak, unauthorized export, or theft of the underlying S3/Garage object store objects yields only AEAD-encrypted ciphertext (`AES-256-GCM`). Without the corresponding DEK in `keys.db`, decryption is computationally infeasible.
- **Data Tampering & Swapping**: Ciphertext frames use AEAD authentication tags with domain-bound associated data (`AAD` containing `file_id`, `chunk_index`, and `is_final` flag). Modifying ciphertext bytes, swapping chunk orders, or truncating chunks immediately triggers integrity failure on read.
- **Crypto-Shredding on Deletion**: When an encrypted file is deleted, its DEK in `keys.db` is securely zeroed in-place (`OPENSSL_cleanse`), checkpointed with `secure_delete = ON`, and transitioned to status `SHREDDED`. Even if ciphertext segments persist in storage snapshots or cold storage, they become permanently unrecoverable.

### What is NOT Protected (Limitations of Local SQLite Key Store)

- **Host Compromise**: Anyone with root or local user access to `$SGRN_DATA_DIR/keys/keys.db` can extract the plaintext DEKs.
- **Platform Administrator Access**: Global admins and host operators can inspect process memory or read `keys.db`.
- **Presigned URLs**: Direct presigned S3 URLs are strictly **blocked** for encrypted files because Garage cannot decrypt data on-the-fly without the DEK.

---

## 2. Key Lifecycle & Architecture

```
[ Domain Policy Check ]
       │ (none / optional / required)
       ▼
[ SqliteKeyProvider ] ─── createPending(object_key) ───► Commit & fsync DEK in keys.db
       │
       ▼
[ AEAD Stream Encrypt ] ─── Encrypt plaintext with AES-256-GCM (1 MiB chunks)
       │
       ▼
[ Garage S3 Upload ] ─── Upload ciphertext to uploads/<hash>/0 (Dedup bypassed!)
       │
       ▼
[ PostgreSQL Transaction ] ─── Insert storage.files (key_id, is_encrypted=true)
       │
       ▼
[ SqliteKeyProvider ] ─── activate(key_id, file_id) ───► Mark ACTIVE in keys.db
```

### Key Provider Interface (`KeyProvider`)

All cryptographic key operations are encapsulated behind the abstract `KeyProvider` interface:

- `createPending(object_key, wrap_scheme)`: Allocates a 32-byte DEK, stores it in state `PENDING`, and fsyncs to WAL before any ciphertext is transmitted.
- `activate(key_id, file_id)`: Binds the pending DEK to the confirmed PostgreSQL `storage.files.id` and transitions status to `ACTIVE`.
- `get(key_id)`: Fetches the DEK. Fails closed if the key is missing, shredded, or wrapped in an unknown scheme. Key bytes are managed exclusively in memory via move-only `Dek` instances that zero themselves on destruction.
- `shred(key_id)`: Overwrites the stored DEK blob with zeros and sets status `SHREDDED`.
- `listForReconcile()`: Returns key metadata **excluding** DEK material for audits.

---

## 3. Key Separation and Backup Requirements

> [!CAUTION]
> **MANDATORY SEPARATION OF BACKUPS**
> The key store backup (`--backup-keys`) must **NEVER** be stored on the same physical server, volume, or cloud backup repository as data backups (`--backup-db` and `--backup-garage`).
>
> Storing the key database alongside data archives nullifies encryption-at-rest. If the key database is lost, **every encrypted file is permanently unrecoverable**.

### Backup Command

```bash
sgrn_datastore --backup-keys [/custom/path/keys-<timestamp>.db]
```

The command uses SQLite online backup API (`sqlite3_backup`) to snapshot `keys.db` safely while the service is running, ensuring atomic 0600 mode permissions.

### Reconciliation Command

```bash
sgrn_datastore --verify-keys
```

Cross-references every encrypted file in PostgreSQL against `keys.db`:

- Flags encrypted files missing their key (fatal data loss).
- Flags orphaned active keys with no file record.
- Identifies uncleaned pending keys from abandoned uploads.

---

## 4. Production Upgrade Path: KMS / HSM

The system is explicitly engineered to decouple storage logic from key management:

1. **`wrap_scheme` Field**: The `file_keys` table records `wrap_scheme` (currently `'none'`). Future providers will support `'aws-kms'`, `'gcp-kms'`, or `'pkcs11'`.
2. **Key Wrapping (KEK)**: Under production schemes, DEKs stored in `keys.db` will be wrapped under a Key Encryption Key (KEK) held in a hardware security module (HSM) or cloud KMS.
3. **Pluggable Providers**: Replace `SqliteKeyProvider` with a remote `KmsKeyProvider` implementing `KeyProvider` without modifying the upload, download, or storage pipelines.
