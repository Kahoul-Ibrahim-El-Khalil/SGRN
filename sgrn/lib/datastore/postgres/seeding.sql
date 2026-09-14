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
  
  ('jsonl', 'application/json', false, 'JSONL Data File',    true),
  
  -- images (webp was missing)
  ('webp', 'image/webp',       true,  'WebP Image',          true),
  -- video (extended)
  ('mpeg', 'video/mpeg',       true,  'MPEG Video',          true),
  ('mpg',  'video/mpeg',       true,  'MPEG Video',          true),
  ('flv',  'video/x-flv',      true,  'Flash Video',         true),
  ('3gp',  'video/3gpp',       true,  '3GPP Video',          true),
  ('mts',  'video/mp2t',       true,  'MPEG Transport Stream', true),
  -- audio (extended)
  ('mid',  'audio/midi',       true,  'MIDI Audio',          true),
  ('midi', 'audio/midi',       true,  'MIDI Audio',          true),
  ('aiff', 'audio/aiff',       true,  'AIFF Audio',          true),
  ('mka',  'audio/x-matroska', true,  'Matroska Audio',      true),
  -- images (extended)
  ('heic', 'image/heic',            true,  'HEIC Image',        true),
  ('heif', 'image/heif',            true,  'HEIF Image',        true),
  ('avif', 'image/avif',            true,  'AVIF Image',        true),
  ('psd',  'image/vnd.adobe.photoshop', true, 'Photoshop Document', true),
  ('tif',  'image/tiff',            true,  'TIFF Image',        true),
  ('dng',  'image/x-adobe-dng',     true,  'Digital Negative',  true),
  ('cr2',  'image/x-canon-cr2',     true,  'Canon RAW Image',   true),
  ('arw',  'image/x-sony-arw',      true,  'Sony RAW Image',    true),
  -- documents / mail (extended)
  ('epub', 'application/epub+zip',  true,  'EPUB eBook',        true),
  ('mobi', 'application/x-mobipocket-ebook', true, 'Mobipocket eBook', true),
  ('djvu', 'image/vnd.djvu',        true,  'DjVu Document',     true),
  ('msg',  'application/vnd.ms-outlook', true, 'Outlook Message', true),
  ('eml',  'message/rfc822',        false, 'Email Message',     true),
  ('ics',  'text/calendar',         false, 'Calendar File',     true),
  ('vcf',  'text/vcard',            false, 'Contact Card',      true),
  -- data (extended)
  ('avro',    'application/avro',                true,  'Avro Data File',       true),
  ('orc',     'application/x-orc',               true,  'ORC Data File',        true),
  ('arrow',   'application/vnd.apache.arrow.file', true, 'Arrow Data File',     true),
  ('feather', 'application/vnd.apache.arrow.file', true, 'Feather Data File',   true),
  ('sqlite',  'application/vnd.sqlite3',         true,  'SQLite Database',      true),
  ('bak',     'application/octet-stream',        false, 'Backup File',          true),
  -- archives / disk images (extended)
  ('xz',  'application/x-xz',                 true, 'XZ Archive',          true),
  ('bz2', 'application/x-bzip2',              true, 'Bzip2 Archive',       true),
  ('iso', 'application/x-iso9660-image',      true, 'ISO Disk Image',      true),
  ('dmg', 'application/x-apple-diskimage',    true, 'Apple Disk Image',    true),
  ('img', 'application/octet-stream',         true, 'Raw Disk Image',      true),
  ('cab', 'application/vnd.ms-cab-compressed', true, 'Cabinet Archive',    true),
  -- code (extended)
  ('cs',    'text/x-csharp', false, 'C# Source Code',   true),
  ('php',   'text/x-php',    false, 'PHP Source Code',  true),
  ('rb',    'text/x-ruby',   false, 'Ruby Source Code', true),
  ('swift', 'text/x-swift',  false, 'Swift Source Code', true),
  ('kt',    'text/x-kotlin', false, 'Kotlin Source Code', true),
  ('scala', 'text/x-scala',  false, 'Scala Source Code', true),
  ('lua',   'text/x-lua',    false, 'Lua Source Code',  true),
  ('pl',    'text/x-perl',   false, 'Perl Source Code', true),
  ('scss',  'text/x-scss',   false, 'SCSS Stylesheet',  true),
  ('less',  'text/x-less',   false, 'LESS Stylesheet',  true),
  ('vue',   'text/x-vue',    false, 'Vue Component',    true),
  ('wasm',  'application/wasm', true, 'WebAssembly Module', true),
  ('map',   'application/json', false, 'Source Map',     true),
  -- fonts
  ('ttf',   'font/ttf',      true,  'TrueType Font',     true),
  ('otf',   'font/otf',      true,  'OpenType Font',     true),
  ('woff',  'font/woff',     true,  'WOFF Font',         true),
  ('woff2', 'font/woff2',    true,  'WOFF2 Font',        true),
  -- 3D / CAD / BIM (extended)
  ('glb',  'model/gltf-binary', true,  'glTF Binary Model', true),
  ('gltf', 'model/gltf+json',   true,  'glTF Model',        true),
  ('jt',   'model/jt',          true,  'JT Model',          true),
  ('3mf',  'model/3mf',         true,  '3MF Model',         true),
  ('ply',  'model/ply',         true,  'PLY Model',         true),
  ('ifc',  'application/x-step', false, 'IFC BIM Model',    true),
  -- GIS
  ('shp',     'application/x-shapefile', true,  'Shapefile',       true),
  ('geojson', 'application/geo+json',    false, 'GeoJSON Data',    true),
  ('kml',     'application/vnd.google-earth.kml+xml', false, 'KML Map Data', true),
  ('gpx',     'application/gpx+xml',     false, 'GPX Track Data',  true),
  -- subtitles
  ('srt', 'application/x-subrip', false, 'SubRip Subtitles', true),
  ('vtt', 'text/vtt',             false, 'WebVTT Subtitles', true),
  -- certificates (nginx/TLS material used by this stack)
  ('pem', 'application/x-pem-file',      false, 'PEM Certificate',     true),
  ('crt', 'application/x-x509-ca-cert',  false, 'X.509 Certificate',   true),
  ('cer', 'application/x-x509-ca-cert',  false, 'X.509 Certificate',   true)
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
