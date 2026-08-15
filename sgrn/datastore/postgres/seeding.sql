-- ============================================================
-- seeding script
-- ============================================================
-- execution order matters — each block depends on the previous.
-- run after schema + views + functions are created.
-- ============================================================
-- 1. base organisation
insert into
  core.organisations (name, description)
values
  (
    'CIC-Moulins Guelma',
    'Milling wheat and producing various forms of pasta, ex-Amor Ben Amor'
  )
on conflict (name) do nothing;

-- 2. domains
insert into
  core.domains (organisation, name)
values
  ('CIC-Moulins Guelma', 'Production'),
  ('CIC-Moulins Guelma', 'Maintenance'),
  ('CIC-Moulins Guelma', 'Logistics'),
  ('CIC-Moulins Guelma', 'Management')
on conflict (organisation, name) do nothing;

-- note: core.roles table has been removed.
-- role is now a text column on core.users with check ('admin' | 'user').
-- no role seeding required.
-- 4. storage formats
insert into
  storage.formats (
    extension,
    mime_type,
    is_compressed,
    description,
    is_allowed
  )
values
  (
    'pdf',
    'application/pdf',
    true,
    'PDF Document',
    true
  ),
  (
    'doc',
    'application/msword',
    true,
    'Microsoft Word Document (Legacy)',
    true
  ),
  (
    'docx',
    'application/vnd.openxmlformats-officedocument.wordprocessingml.document',
    true,
    'Microsoft Word Document',
    true
  ),
  (
    'odt',
    'application/vnd.oasis.opendocument.text',
    true,
    'OpenDocument Text',
    true
  ),
  (
    'rtf',
    'application/rtf',
    false,
    'Rich Text Format',
    true
  ),
  (
    'txt',
    'text/plain',
    false,
    'Plain Text File',
    true
  ),
  (
    'md',
    'text/markdown',
    false,
    'Markdown Document',
    true
  ),
  (
    'xls',
    'application/vnd.ms-excel',
    true,
    'Excel Spreadsheet (Legacy)',
    true
  ),
  (
    'xlsx',
    'application/vnd.openxmlformats-officedocument.spreadsheetml.sheet',
    true,
    'Excel Spreadsheet',
    true
  ),
  (
    'ods',
    'application/vnd.oasis.opendocument.spreadsheet',
    true,
    'OpenDocument Spreadsheet',
    true
  ),
  (
    'csv',
    'text/csv',
    false,
    'Comma-separated Values',
    true
  ),
  (
    'tsv',
    'text/tab-separated-values',
    false,
    'Tab-separated Values',
    true
  ),
  (
    'ppt',
    'application/vnd.ms-powerpoint',
    true,
    'PowerPoint Presentation (Legacy)',
    true
  ),
  (
    'pptx',
    'application/vnd.openxmlformats-officedocument.presentationml.presentation',
    true,
    'PowerPoint Presentation',
    true
  ),
  (
    'odp',
    'application/vnd.oasis.opendocument.presentation',
    true,
    'OpenDocument Presentation',
    true
  ),
  ('png', 'image/png', true, 'PNG Image', true),
  ('jpg', 'image/jpeg', true, 'JPEG Image', true),
  ('jpeg', 'image/jpeg', true, 'JPEG Image', true),
  ('gif', 'image/gif', true, 'GIF Image', true),
  ('bmp', 'image/bmp', true, 'Bitmap Image', true),
  ('tiff', 'image/tiff', true, 'TIFF Image', true),
  (
    'svg',
    'image/svg+xml',
    false,
    'Scalable Vector Graphic',
    true
  ),
  ('ico', 'image/x-icon', false, 'Icon File', true),
  (
    'zip',
    'application/zip',
    true,
    'ZIP Archive',
    true
  ),
  (
    'rar',
    'application/vnd.rar',
    true,
    'RAR Archive',
    true
  ),
  (
    '7z',
    'application/x-7z-compressed',
    true,
    '7-Zip Archive',
    true
  ),
  (
    'tar',
    'application/x-tar',
    true,
    'TAR Archive',
    true
  ),
  (
    'txt',
    'text/plain',
    false,
    'Plain Text File',
    true
  ),
  (
    'db',
    'text/x-s7-db',
    false,
    'Siemens S7 Data Block',
    true
  ),
  (
    'scl',
    'text/x-scl',
    false,
    'Siemens Structured Control Language',
    true
  ),
  (
    'udt',
    'text/x-s7-udt',
    false,
    'Siemens User Defined Type',
    true
  ),
  (
    'zst',
    'application/zstd',
    true,
    'Zstd Compressed Archive',
    true
  ),
  (
    'gz',
    'application/gzip',
    true,
    'Gzip Compressed File',
    true
  ),
  (
    'br',
    'application/brotli',
    true,
    'Brotli Compressed File',
    true
  ),
  (
    'xml',
    'application/xml',
    false,
    'XML Document',
    true
  ),
  (
    'yaml',
    'application/x-yaml',
    false,
    'YAML File',
    true
  ),
  (
    'yml',
    'application/x-yaml',
    false,
    'YAML File',
    true
  ),
  (
    'ini',
    'text/plain',
    false,
    'Configuration File',
    true
  ),
  ('log', 'text/plain', false, 'Log File', true),
  (
    'cfg',
    'text/plain',
    false,
    'Configuration File (Generic)',
    true
  ),
  (
    'conf',
    'text/plain',
    false,
    'Configuration File',
    true
  ),
  (
    'dat',
    'application/octet-stream',
    false,
    'Generic Data File',
    true
  ),
  ('lst', 'text/plain', false, 'List File', true),
  (
    'properties',
    'text/plain',
    false,
    'Java/ini Style Config File',
    true
  ),
  (
    'env',
    'text/plain',
    false,
    'Environment Variables File',
    true
  ),
  ('c', 'text/x-csrc', false, 'C Source Code', true),
  (
    'cpp',
    'text/x-c++src',
    false,
    'C++ Source Code',
    true
  ),
  ('h', 'text/x-chdr', false, 'C Header File', true),
  (
    'hpp',
    'text/x-c++hdr',
    false,
    'C++ Header File',
    true
  ),
  (
    'py',
    'text/x-python',
    false,
    'Python Script',
    true
  ),
  (
    'js',
    'application/javascript',
    false,
    'JavaScript File',
    true
  ),
  (
    'ts',
    'application/typescript',
    false,
    'TypeScript File',
    true
  ),
  ('html', 'text/html', false, 'HTML Document', true),
  ('css', 'text/css', false, 'CSS Stylesheet', true),
  (
    'sql',
    'application/sql',
    false,
    'SQL Script',
    true
  ),
  (
    'sh',
    'application/x-sh',
    false,
    'Shell Script',
    true
  ),
  (
    'bat',
    'application/x-msdos-program',
    false,
    'Batch File',
    true
  ),
  (
    'ps1',
    'application/x-powershell',
    false,
    'PowerShell Script',
    true
  ),
  (
    'java',
    'text/x-java-source',
    false,
    'Java Source Code',
    true
  ),
  ('go', 'text/x-go', false, 'Go Source Code', true),
  (
    'rs',
    'text/x-rustsrc',
    false,
    'Rust Source Code',
    true
  ),
  (
    'dwg',
    'image/vnd.dwg',
    true,
    'AutoCAD Drawing Database File',
    true
  ),
  (
    'dxf',
    'image/vnd.dxf',
    true,
    'Drawing Exchange Format',
    true
  ),
  (
    'stl',
    'model/stl',
    false,
    'Stereolithography 3D Model',
    true
  ),
  (
    'step',
    'application/step',
    false,
    'STEP 3D Model File',
    true
  ),
  (
    'stp',
    'application/step',
    false,
    'STEP 3D Model File',
    true
  ),
  (
    'iges',
    'model/iges',
    false,
    'IGES 3D Model File',
    true
  ),
  (
    'igs',
    'model/iges',
    false,
    'IGES 3D Model File',
    true
  ),
  (
    '3ds',
    'image/x-3ds',
    true,
    '3D Studio Model',
    true
  ),
  (
    'obj',
    'model/obj',
    false,
    'Wavefront 3D Object File',
    true
  ),
  (
    'fbx',
    'application/octet-stream',
    true,
    'Autodesk FBX 3D Model',
    true
  ),
  (
    'skp',
    'application/vnd.sketchup.skp',
    true,
    'SketchUp Model File',
    true
  ),
  (
    'sldprt',
    'application/sldworks',
    true,
    'SolidWorks Part File',
    true
  ),
  (
    'sldasm',
    'application/sldworks',
    true,
    'SolidWorks Assembly File',
    true
  ),
  (
    'prt',
    'application/octet-stream',
    true,
    'Generic CAD Part File',
    true
  ),
  (
    'catpart',
    'application/octet-stream',
    true,
    'CATIA Part File',
    true
  ),
  (
    'catproduct',
    'application/octet-stream',
    true,
    'CATIA Product Assembly',
    true
  ),
  (
    'xlsm',
    'application/vnd.ms-excel.sheet.macroenabled.12',
    true,
    'Excel Macro-Enabled Workbook',
    true
  ),
  (
    'dbf',
    'application/x-dbf',
    false,
    'Database File',
    true
  ),
  (
    'parquet',
    'application/octet-stream',
    true,
    'Apache Parquet Data File',
    true
  ),
  (
    'tex',
    'application/x-tex',
    false,
    'LaTeX Document',
    true
  ),
  (
    'rst',
    'text/x-rst',
    false,
    'reStructuredText File',
    true
  ),
  (
    'toml',
    'application/toml',
    false,
    'TOML File',
    true
  ),
  -- audio
  ('mp3',  'audio/mpeg',       true,  'MP3 Audio',           true),
  ('m4a',  'audio/mp4',        true,  'M4A Audio',           true),
  ('wav',  'audio/wav',        false, 'WAV Audio',           true),
  ('ogg',  'audio/ogg',        true,  'OGG Audio',           true),
  ('flac', 'audio/flac',       true,  'FLAC Lossless Audio', true),
  ('aac',  'audio/aac',        true,  'AAC Audio',           true),
  ('opus', 'audio/opus',       true,  'Opus Audio',          true),
  -- video
  ('mp4',  'video/mp4',        true,  'MPEG-4 Video',        true),
  ('webm', 'video/webm',       true,  'WebM Video',          true),
  ('mkv',  'video/x-matroska', true,  'Matroska Video',      true),
  ('mov',  'video/quicktime',  true,  'QuickTime Video',     true),
  ('avi',  'video/x-msvideo',  true,  'AVI Video',           true),
  ('wmv',  'video/x-ms-wmv',   true,  'Windows Media Video', true),
  -- data
  ('json', 'application/json', false, 'JSON Data File',      true),
  -- images (webp was missing)
  ('webp', 'image/webp',       true,  'WebP Image',          true)
on conflict (extension) do nothing;

-- 5. initial admin user
-- password 'adminroot' is automatically hashed by trg_hash_password.
-- role is now a text column — no fk lookup needed.
insert into
  core.users (
    organisation,
    first_name,
    family_name,
    email,
    password,
    role,
    status,
    is_active
  )
values
  (
    'CIC-Moulins Guelma',
    'System',
    'Admin',
    'admin@local.com',
    crypt ('adminroot', gen_salt ('bf', 4)),
    'admin',
    'active',
    true
  )
on conflict (email) do nothing;
