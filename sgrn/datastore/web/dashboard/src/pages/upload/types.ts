//src/types/upload.ts
export interface UploadProgress {
    [file_name: string]: number;
}

export type { UploadResult, UploadConstraints } from "@sgrn/types";

export interface ExtensionRecord {
    extension: string;
    is_compressed: boolean;
    mime_type: string;
}
