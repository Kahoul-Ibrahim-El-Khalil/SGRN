#pragma once

#include <drogon/HttpTypes.h>
#include <string>

namespace sgrn
{

struct HttpError {
    std::string code_name;
    std::string message;
    drogon::HttpStatusCode status;
};

// =========================================================
// Generic Errors
// =========================================================
enum class GenericApiError { Unknown, InternalServerError, BadRequest, NotFound, Unauthorized, Forbidden, ValidationFailed };

inline HttpError makeHttpError(GenericApiError e) {
    switch (e) {
        case GenericApiError::Unknown:
            return {"SGRN_ERROR_UNKNOWN", "An unknown error occurred.", drogon::k500InternalServerError};
        case GenericApiError::InternalServerError:
            return {"SGRN_ERROR_INTERNAL_SERVER_ERROR", "An internal server error occurred.", drogon::k500InternalServerError};
        case GenericApiError::BadRequest:
            return {"SGRN_ERROR_BAD_REQUEST", "Bad request.", drogon::k400BadRequest};
        case GenericApiError::NotFound:
            return {"SGRN_ERROR_NOT_FOUND", "Resource not found.", drogon::k404NotFound};
        case GenericApiError::Unauthorized:
            return {"SGRN_ERROR_UNAUTHORIZED", "Unauthorized access.", drogon::k401Unauthorized};
        case GenericApiError::Forbidden:
            return {"SGRN_ERROR_FORBIDDEN", "Access to the resource is forbidden.", drogon::k403Forbidden};
        case GenericApiError::ValidationFailed:
            return {"SGRN_ERROR_VALIDATION_FAILED", "Input validation failed.", drogon::k400BadRequest};
    }
    return {"SGRN_ERROR_UNKNOWN", "An unknown error occurred.", drogon::k500InternalServerError};
}

// =========================================================
// Auth API Errors
// =========================================================
enum class AuthApiError { InvalidCredentials, TokenExpired, MissingSessionToken, SessionExpired, SessionInvalid };

inline HttpError makeHttpError(AuthApiError e) {
    switch (e) {
        case AuthApiError::InvalidCredentials:
            return {"AUTH_API_INVALID_CREDENTIALS", "Authentication failed: Invalid credentials.", drogon::k401Unauthorized};
        case AuthApiError::TokenExpired:
            return {"AUTH_API_TOKEN_EXPIRED", "Authentication failed: Token expired.", drogon::k401Unauthorized};
        case AuthApiError::MissingSessionToken:
            return {"AUTH_API_MISSING_SESSION_TOKEN", "Authentication failed: Missing session token.", drogon::k401Unauthorized};
        case AuthApiError::SessionExpired:
            return {"AUTH_API_SESSION_EXPIRED", "Authentication failed: Session expired.", drogon::k401Unauthorized};
        case AuthApiError::SessionInvalid:
            return {"AUTH_API_SESSION_INVALID", "Authentication failed: Session invalid.", drogon::k401Unauthorized};
    }
    return {"AUTH_API_UNKNOWN", "Authentication failed.", drogon::k500InternalServerError};
}

// =========================================================
// Admin API Errors
// =========================================================
enum class AdminApiError {
    UserAlreadyExists,
    InvalidUserId,
    InvalidPayload,
    FailedToOpenEndpointsFile,
    EndpointsFileEmpty,
    DbUnavailable,
    DbError
};

inline HttpError makeHttpError(AdminApiError e) {
    switch (e) {
        case AdminApiError::UserAlreadyExists:
            return {"ADMIN_API_USER_ALREADY_EXISTS", "Admin API error: User with provided ID already exists.", drogon::k409Conflict};
        case AdminApiError::InvalidUserId:
            return {"ADMIN_API_INVALID_USER_ID", "Admin API error: Invalid user ID.", drogon::k400BadRequest};
        case AdminApiError::InvalidPayload:
            return {"ADMIN_API_INVALID_PAYLOAD", "Admin API error: Invalid request payload.", drogon::k400BadRequest};
        case AdminApiError::FailedToOpenEndpointsFile:
            return {"ADMIN_API_FAILED_TO_OPEN_ENDPOINTS_FILE", "Admin API error: Failed to open endpoints configuration file.",
                drogon::k500InternalServerError};
        case AdminApiError::EndpointsFileEmpty:
            return {"ADMIN_API_ENDPOINTS_FILE_EMPTY", "Admin API error: Endpoints configuration file is empty.",
                drogon::k500InternalServerError};
        case AdminApiError::DbUnavailable:
            return {"ADMIN_API_DB_UNAVAILABLE", "Admin API error: Database unavailable.", drogon::k503ServiceUnavailable};
        case AdminApiError::DbError:
            return {
                "ADMIN_API_DB_ERROR", "Admin API error: Database operation failed.", drogon::k409Conflict}; // Conflict used in old switch
    }
    return {"ADMIN_API_UNKNOWN", "Admin API error.", drogon::k500InternalServerError};
}

// =========================================================
// Query API Errors
// =========================================================
enum class QueryApiError { InvalidOrgId, DbUnavailable, DbError, InvalidUserId, UserNotFound };

inline HttpError makeHttpError(QueryApiError e) {
    switch (e) {
        case QueryApiError::InvalidOrgId:
            return {"QUERY_API_INVALID_ORG_ID", "Query API error: Invalid or missing organisation ID.", drogon::k400BadRequest};
        case QueryApiError::DbUnavailable:
            return {"QUERY_API_DB_UNAVAILABLE", "Query API error: Database connection unavailable.", drogon::k500InternalServerError};
        case QueryApiError::DbError:
            return {"QUERY_API_DB_ERROR", "Query API error: Database operation failed.", drogon::k500InternalServerError};
        case QueryApiError::InvalidUserId:
            return {"QUERY_API_INVALID_USER_ID", "Query API error: Invalid or missing user ID.", drogon::k400BadRequest};
        case QueryApiError::UserNotFound:
            return {"QUERY_API_USER_NOT_FOUND", "Query API error: User not found.", drogon::k404NotFound};
    }
    return {"QUERY_API_UNKNOWN", "Query API error.", drogon::k500InternalServerError};
}

// =========================================================
// Resource API Errors
// =========================================================
enum class ResourceApiError { WasmFileNotFound, WasmOpenFailed };

inline HttpError makeHttpError(ResourceApiError e) {
    switch (e) {
        case ResourceApiError::WasmFileNotFound:
            return {"RESOURCE_API_WASM_FILE_NOT_FOUND", "Resource API error: WebAssembly file not found.", drogon::k404NotFound};
        case ResourceApiError::WasmOpenFailed:
            return {"RESOURCE_API_WASM_OPEN_FAILED", "Resource API error: Failed to open WebAssembly file.", drogon::k404NotFound};
    }
    return {"RESOURCE_API_UNKNOWN", "Resource API error.", drogon::k500InternalServerError};
}

// =========================================================
// Download API Errors
// =========================================================
enum class DownloadApiError {
    MissingIdParam,
    InvalidIdFormat,
    InitFailed,
    MissingToken,
    LinkExpiredOrNotFound,
    ServerError,
    PermissionDenied,
    FileNotFound
};

inline HttpError makeHttpError(DownloadApiError e) {
    switch (e) {
        case DownloadApiError::MissingIdParam:
            return {"DOWNLOAD_API_MISSING_ID_PARAM", "Download API error: Missing ID parameter for file.", drogon::k400BadRequest};
        case DownloadApiError::InvalidIdFormat:
            return {"DOWNLOAD_API_INVALID_ID_FORMAT", "Download API error: Invalid file ID format.", drogon::k400BadRequest};
        case DownloadApiError::InitFailed:
            return {"DOWNLOAD_API_INIT_FAILED", "Download API error: Failed to initialize download.", drogon::k500InternalServerError};
        case DownloadApiError::MissingToken:
            return {"DOWNLOAD_API_MISSING_TOKEN", "Download API error: Missing token.", drogon::k400BadRequest};
        case DownloadApiError::LinkExpiredOrNotFound:
            return {"DOWNLOAD_API_LINK_EXPIRED_OR_NOT_FOUND", "Download API error: Path not found or link expired.", drogon::k404NotFound};
        case DownloadApiError::ServerError:
            return {
                "DOWNLOAD_API_SERVER_ERROR", "Download API error: Internal server error during download.", drogon::k500InternalServerError};
        case DownloadApiError::PermissionDenied:
            return {"DOWNLOAD_HANDLER_PERMISSION_DENIED", "Download failed: Permission denied.", drogon::k403Forbidden};
        case DownloadApiError::FileNotFound:
            return {"DOWNLOAD_HANDLER_FILE_NOT_FOUND", "Download failed: File not found.", drogon::k404NotFound};
    }
    return {"DOWNLOAD_API_UNKNOWN", "Download API error.", drogon::k500InternalServerError};
}

// =========================================================
// File Metadata API Errors
// =========================================================
enum class FileMetadataApiError {
    MissingSessionToken,
    InvalidOrExpiredSession,
    CorruptSessionData,
    AuthInternalError,
    MissingIdParameter,
    InvalidIdFormat,
    DbUnavailable,
    FileNotFound,
    InternalServerError,
    MissingHashParameter,
    NoFilesFound,
    MissingNameParameter,
    MissingSubmissionIdParameter,
    InvalidSubmissionIdFormat,
    MissingExtensionParameter,
    MissingSizeParameters,
    InvalidSizeParametersFormat,
    MissingTimestampRangeParameters,
    MissingTimestampParameter
};

inline HttpError makeHttpError(FileMetadataApiError e) {
    switch (e) {
        case FileMetadataApiError::MissingSessionToken:
            return {"FILEMETADATA_API_MISSING_SESSION_TOKEN", "File Metadata API error: Missing session token.", drogon::k401Unauthorized};
        case FileMetadataApiError::InvalidOrExpiredSession:
            return {"FILEMETADATA_API_INVALID_OR_EXPIRED_SESSION", "File Metadata API error: Invalid or expired session.",
                drogon::k401Unauthorized};
        case FileMetadataApiError::CorruptSessionData:
            return {
                "FILEMETADATA_API_CORRUPT_SESSION_DATA", "File Metadata API error: Corrupt session data.", drogon::k500InternalServerError};
        case FileMetadataApiError::AuthInternalError:
            return {"FILEMETADATA_API_AUTH_INTERNAL_ERROR", "File Metadata API error: Internal server error during authentication.",
                drogon::k500InternalServerError};
        case FileMetadataApiError::MissingIdParameter:
            return {"FILEMETADATA_API_MISSING_ID_PARAMETER", "File Metadata API error: Missing ID parameter.", drogon::k400BadRequest};
        case FileMetadataApiError::InvalidIdFormat:
            return {"FILEMETADATA_API_INVALID_ID_FORMAT", "File Metadata API error: Invalid ID parameter format.", drogon::k400BadRequest};
        case FileMetadataApiError::DbUnavailable:
            return {"FILEMETADATA_API_DB_UNAVAILABLE", "File Metadata API error: Database unavailable.", drogon::k500InternalServerError};
        case FileMetadataApiError::FileNotFound:
            return {"FILEMETADATA_API_FILE_NOT_FOUND", "File Metadata API error: File not found.", drogon::k404NotFound};
        case FileMetadataApiError::InternalServerError:
            return {"FILEMETADATA_API_INTERNAL_SERVER_ERROR", "File Metadata API error: Internal server error.",
                drogon::k500InternalServerError};
        case FileMetadataApiError::MissingHashParameter:
            return {"FILEMETADATA_API_MISSING_HASH_PARAMETER", "File Metadata API error: Missing hash parameter.", drogon::k400BadRequest};
        case FileMetadataApiError::NoFilesFound:
            return {"FILEMETADATA_API_NO_FILES_FOUND", "File Metadata API error: No files found.", drogon::k404NotFound};
        case FileMetadataApiError::MissingNameParameter:
            return {"FILEMETADATA_API_MISSING_NAME_PARAMETER", "File Metadata API error: Missing name parameter.", drogon::k400BadRequest};
        case FileMetadataApiError::MissingSubmissionIdParameter:
            return {"FILEMETADATA_API_MISSING_SUBMISSION_ID_PARAMETER", "File Metadata API error: Missing submission ID parameter.",
                drogon::k400BadRequest};
        case FileMetadataApiError::InvalidSubmissionIdFormat:
            return {"FILEMETADATA_API_INVALID_SUBMISSION_ID_FORMAT", "File Metadata API error: Invalid submission ID format.",
                drogon::k400BadRequest};
        case FileMetadataApiError::MissingExtensionParameter:
            return {"FILEMETADATA_API_MISSING_EXTENSION_PARAMETER", "File Metadata API error: Missing extension parameter.",
                drogon::k400BadRequest};
        case FileMetadataApiError::MissingSizeParameters:
            return {"FILEMETADATA_API_MISSING_SIZE_PARAMETERS", "File Metadata API error: Missing size parameters (min, max).",
                drogon::k400BadRequest};
        case FileMetadataApiError::InvalidSizeParametersFormat:
            return {"FILEMETADATA_API_INVALID_SIZE_PARAMETERS_FORMAT", "File Metadata API error: Invalid size parameters format.",
                drogon::k400BadRequest};
        case FileMetadataApiError::MissingTimestampRangeParameters:
            return {"FILEMETADATA_API_MISSING_TIMESTAMP_RANGE_PARAMETERS",
                "File Metadata API error: Missing timestamp range parameters (start, end).", drogon::k400BadRequest};
        case FileMetadataApiError::MissingTimestampParameter:
            return {"FILEMETADATA_API_MISSING_TIMESTAMP_PARAMETER", "File Metadata API error: Missing timestamp parameter.",
                drogon::k400BadRequest};
    }
    return {"FILEMETADATA_API_UNKNOWN", "File Metadata API error.", drogon::k500InternalServerError};
}

// =========================================================
// Upload API Errors
// =========================================================
enum class UploadApiError {
    InvalidHookSecret,
    InvalidJson,
    InvalidJsonStructure,
    MissingUploadKey,
    InvalidUploadKey,
    RedisError,
    DuplicateFile,
    InvalidHashFormat,
    DbDeduplicationError,
    ValidationError,
    MissingUserDeptId,
    InvalidIdFormat,
    UploadFileNotFound,
    FileProcessingError,
    InvalidOriginalSizeFormat,
    FilesystemError,
    DbUploadError,
    InternalServerError,
    DbUnavailable,
    FetchExtensionsFailed,
    InvalidFileType,
    MaxSizeExceeded
};

inline HttpError makeHttpError(UploadApiError e) {
    switch (e) {
        case UploadApiError::InvalidHookSecret:
            return {"UPLOAD_API_INVALID_HOOK_SECRET", "Upload API error: Invalid hook secret.", drogon::k403Forbidden};
        case UploadApiError::InvalidJson:
            return {"UPLOAD_API_INVALID_JSON", "Upload API error: Invalid JSON payload.", drogon::k400BadRequest};
        case UploadApiError::InvalidJsonStructure:
            return {"UPLOAD_API_INVALID_JSON_STRUCTURE", "Upload API error: Invalid JSON structure.", drogon::k400BadRequest};
        case UploadApiError::MissingUploadKey:
            return {"UPLOAD_API_MISSING_UPLOAD_KEY", "Upload API error: Missing SGRN-Upload-Key.", drogon::k401Unauthorized};
        case UploadApiError::InvalidUploadKey:
            return {"UPLOAD_API_INVALID_UPLOAD_KEY", "Upload API error: Invalid upload key.", drogon::k401Unauthorized};
        case UploadApiError::RedisError:
            return {"UPLOAD_API_REDIS_ERROR", "Upload API error: Redis operation failed.", drogon::k500InternalServerError};
        case UploadApiError::DuplicateFile:
            return {"UPLOAD_API_DUPLICATE_FILE", "Upload API error: File already exists.", drogon::k409Conflict};
        case UploadApiError::InvalidHashFormat:
            return {"UPLOAD_API_INVALID_HASH_FORMAT", "Upload API error: Invalid hash format.", drogon::k400BadRequest};
        case UploadApiError::DbDeduplicationError:
            return {"UPLOAD_API_DB_DEDUPLICATION_ERROR", "Upload API error: Database error during deduplication check.",
                drogon::k503ServiceUnavailable};
        case UploadApiError::ValidationError:
            return {"UPLOAD_API_VALIDATION_ERROR", "Upload API error: Internal error during upload validation.",
                drogon::k500InternalServerError};
        case UploadApiError::MissingUserDeptId:
            return {"UPLOAD_API_MISSING_USER_DEPT_ID", "Upload API error: Missing user ID or department ID.", drogon::k400BadRequest};
        case UploadApiError::InvalidIdFormat:
            return {"UPLOAD_API_INVALID_ID_FORMAT", "Upload API error: Invalid user ID or department ID format.", drogon::k400BadRequest};
        case UploadApiError::UploadFileNotFound:
            return {"UPLOAD_API_UPLOAD_FILE_NOT_FOUND", "Upload API error: Upload file not found.", drogon::k404NotFound};
        case UploadApiError::FileProcessingError:
            return {
                "UPLOAD_API_FILE_PROCESSING_ERROR", "Upload API error: Failed to process uploaded file.", drogon::k500InternalServerError};
        case UploadApiError::InvalidOriginalSizeFormat:
            return {"UPLOAD_API_INVALID_ORIGINAL_SIZE_FORMAT", "Upload API error: Invalid original size format.", drogon::k400BadRequest};
        case UploadApiError::FilesystemError:
            return {"UPLOAD_API_FILESYSTEM_ERROR", "Upload API error: Filesystem operation failed.", drogon::k500InternalServerError};
        case UploadApiError::DbUploadError:
            return {"UPLOAD_API_DB_UPLOAD_ERROR", "Upload API error: Database error during upload.", drogon::k500InternalServerError};
        case UploadApiError::InternalServerError:
            return {"UPLOAD_API_INTERNAL_SERVER_ERROR", "Upload API error: Internal server error during upload.",
                drogon::k500InternalServerError};
        case UploadApiError::DbUnavailable:
            return {"UPLOAD_API_DB_UNAVAILABLE", "Upload API error: Database unavailable.", drogon::k503ServiceUnavailable};
        case UploadApiError::FetchExtensionsFailed:
            return {"UPLOAD_API_FETCH_EXTENSIONS_FAILED", "Upload API error: Failed to fetch allowed extensions.",
                drogon::k500InternalServerError};
        case UploadApiError::InvalidFileType:
            return {"UPLOAD_HANDLER_INVALID_FILE_TYPE", "Upload failed: Invalid file type.", drogon::k400BadRequest};
        case UploadApiError::MaxSizeExceeded:
            return {"UPLOAD_HANDLER_MAX_SIZE_EXCEEDED", "Upload failed: Maximum file size exceeded.", drogon::k400BadRequest};
    }
    return {"UPLOAD_API_UNKNOWN", "Upload API error.", drogon::k500InternalServerError};
}

// =========================================================
// VFS API Errors
// =========================================================
enum class VfsApiError {
    MissingIdOrPathParameter,
    MissingJsonBody,
    RequiredFieldsMissing,
    InvalidPathOrNameCombination,
    MethodNotAllowed,
    NoFileUploaded,
    InvalidTargetFolderId,
    MissingParentId,
    MissingPatternParameter,
    MissingHashParameter,
    MissingSrcOrDestParameter,
    MissingIdParameter,
    NotFound,
    AlreadyExists,
    InvalidPath,
    InvalidParameter,
    NotEmpty,
    CircularReference,
    PermissionDenied,
    DbUnavailable,
    DbQueryFailed,
    DbConstraintViolation,
    OperationFailedGeneric,
    PathNotFound
};

inline HttpError makeHttpError(VfsApiError e) {
    switch (e) {
        case VfsApiError::MissingIdOrPathParameter:
            return {"VFS_API_MISSING_ID_OR_PATH_PARAMETER", "VFS API error: Missing ID or path parameter.", drogon::k400BadRequest};
        case VfsApiError::MissingJsonBody:
            return {"VFS_API_MISSING_JSON_BODY", "VFS API error: Missing JSON body.", drogon::k400BadRequest};
        case VfsApiError::RequiredFieldsMissing:
            return {"VFS_API_REQUIRED_FIELDS_MISSING", "VFS API error: Required fields are missing.", drogon::k400BadRequest};
        case VfsApiError::InvalidPathOrNameCombination:
            return {"VFS_API_INVALID_PATH_OR_NAME_COMBINATION", "VFS API error: Invalid path or name combination.", drogon::k400BadRequest};
        case VfsApiError::MethodNotAllowed:
            return {"VFS_API_METHOD_NOT_ALLOWED", "VFS API error: Method not allowed.", drogon::k405MethodNotAllowed};
        case VfsApiError::NoFileUploaded:
            return {"VFS_API_NO_FILE_UPLOADED", "VFS API error: No file uploaded or parse error.", drogon::k400BadRequest};
        case VfsApiError::InvalidTargetFolderId:
            return {"VFS_API_INVALID_TARGET_FOLDER_ID", "VFS API error: Invalid target folder ID.", drogon::k400BadRequest};
        case VfsApiError::MissingParentId:
            return {"VFS_API_MISSING_PARENT_ID", "VFS API error: Missing parent ID (targetFolderId).", drogon::k400BadRequest};
        case VfsApiError::MissingPatternParameter:
            return {"VFS_API_MISSING_PATTERN_PARAMETER", "VFS API error: Missing pattern parameter.", drogon::k400BadRequest};
        case VfsApiError::MissingHashParameter:
            return {"VFS_API_MISSING_HASH_PARAMETER", "VFS API error: Missing hash parameter.", drogon::k400BadRequest};
        case VfsApiError::MissingSrcOrDestParameter:
            return {
                "VFS_API_MISSING_SRC_OR_DEST_PARAMETER", "VFS API error: Missing source or destination parameter.", drogon::k400BadRequest};
        case VfsApiError::MissingIdParameter:
            return {"VFS_API_MISSING_ID_PARAMETER", "VFS API error: Missing ID parameter.", drogon::k400BadRequest};
        case VfsApiError::NotFound:
            return {"VFS_API_NOT_FOUND", "VFS API error: Resource not found.", drogon::k404NotFound};
        case VfsApiError::AlreadyExists:
            return {"VFS_API_ALREADY_EXISTS", "VFS API error: Resource already exists.", drogon::k409Conflict};
        case VfsApiError::InvalidPath:
            return {"VFS_API_INVALID_PATH", "VFS API error: Invalid path.", drogon::k400BadRequest};
        case VfsApiError::InvalidParameter:
            return {"VFS_API_INVALID_PARAMETER", "VFS API error: Invalid parameter.", drogon::k400BadRequest};
        case VfsApiError::NotEmpty:
            return {"VFS_API_NOT_EMPTY", "VFS API error: Directory not empty.", drogon::k400BadRequest};
        case VfsApiError::CircularReference:
            return {"VFS_API_CIRCULAR_REFERENCE", "VFS API error: Circular reference detected.", drogon::k400BadRequest};
        case VfsApiError::PermissionDenied:
            return {"VFS_API_PERMISSION_DENIED", "VFS API error: Permission denied.", drogon::k403Forbidden};
        case VfsApiError::DbUnavailable:
            return {"VFS_API_DB_UNAVAILABLE", "VFS API error: Database unavailable.", drogon::k503ServiceUnavailable};
        case VfsApiError::DbQueryFailed:
            return {"VFS_API_DB_QUERY_FAILED", "VFS API error: Database query failed.", drogon::k500InternalServerError};
        case VfsApiError::DbConstraintViolation:
            return {"VFS_API_DB_CONSTRAINT_VIOLATION", "VFS API error: Database constraint violation.", drogon::k500InternalServerError};
        case VfsApiError::OperationFailedGeneric:
            return {"VFS_API_OPERATION_FAILED_GENERIC", "VFS API error: VFS operation failed.", drogon::k500InternalServerError};
        case VfsApiError::PathNotFound:
            return {"VFS_PATH_NOT_FOUND", "Virtual file system path not found.", drogon::k404NotFound};
    }
    return {"VFS_API_UNKNOWN", "VFS API error.", drogon::k500InternalServerError};
}

} // namespace sgrn
