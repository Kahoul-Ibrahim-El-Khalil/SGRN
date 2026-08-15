# North-Bound Interfaces

The North-Bound Interfaces of the SGRN system are primarily exposed through a RESTful API, consumed by clients such as the `AutomatedServiceClient.ts`. These interfaces facilitate authentication, session management, and file storage operations.

## API Endpoints

The core API endpoints are defined in `clients/sgrn/AutomatedServiceClient.ts` and are accessed via HTTP requests. All API paths are prefixed with `/api/v1/`.

### Authentication Endpoints

*   `POST /api/v1/auth/automated-service/signin`: Authenticates an automated service using provided credentials (token and secret) and returns a JSON Web Token (JWT) for subsequent authenticated requests. This also establishes a session.
*   `POST /api/v1/auth/automated-service/signout`: Terminates the current session and invalidates the JWT.
*   `GET /api/v1/auth/automated-service/session`: Retrieves the current automated service session details.

### Storage Endpoints

*   `GET /api/v1/storage/info`: Provides information on storage constraints, such as maximum file size and allowed file extensions. This endpoint does not require authentication.
*   `GET /api/v1/storage/automated-service/files?path=<virtual_path>`: Downloads a file from the virtual file system. Requires authentication.
*   `POST /api/v1/storage/automated-service/files?path=<virtual_path>`: Uploads a file to a specified virtual path in the file system. Supports ZSTD compression. Requires authentication.

## Client Interaction

The `AutomatedServiceClient` handles the complexities of API interaction, including:

*   **Error Handling:** Provides structured error responses, categorizing issues into `Authentication`, `Authorization`, `Network`, `FileSystem`, and `Unknown` scopes based on HTTP status codes and response content.
*   **Content Types:** Primarily uses `application/json` for request bodies and responses, with `multipart/form-data` and `application/zstd` for file uploads.
*   **File Transfer:** Offers methods for downloading files as raw `Response` objects or `ArrayBuffer`s, and uploading files from `File`/`Blob` objects or directly from the local disk.
