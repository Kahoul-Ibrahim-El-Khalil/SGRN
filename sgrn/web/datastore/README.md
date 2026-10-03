# 🌐 SGRN Datastore Web Dashboard

The **SGRN Datastore Web Application** is a React + TypeScript + Vite frontend that provides a desktop-class user interface for managing SGRN Datastore workspaces, object storage, active sessions, and system telemetry.

---

## 🎨 Key Features

### 📁 Storage & Drive UI
- **Virtual File Explorer:** Grid and List view modes with breadcrumb navigation, search filtering, and multi-selection support.
- **Resumable Chunked Uploads:** Automatic thresholding (files $\ge$ 5MB automatically switch to multi-chunk resumable upload).
- **Floating Upload Progress Bar:** Real-time bottom-right progress panel displaying active/completed uploads with animated gradient progress bars, chunk counts, uploaded size, and status indicators.
- **Resumable Downloads:** Native browser `HTTP Range` request support for resuming interrupted file downloads.
- **Drag & Drop:** Full-screen drop overlay for seamless single-file, multi-file, and folder structure uploads.

### 🛡️ Admin & Telemetry Dashboard
- **Live User Sessions:** Metaprobe table displaying active authenticated user sessions, auth modes (`password`, `sso`, `token`), domain, and IP addresses.
- **Webhook Management:** Real-time interface for registering and deleting event webhooks (`user.signin`, `user.signout`).
- **Storage Metrics & Quotas:** Storage breakdown visualizer, orphan scanner, and domain quota allocation overview.

---

## 🚀 Getting Started

### Development Server
```bash
npm install
npm run dev
```

### Production Build
```bash
npm run build
```

---

## 🛠️ Architecture & Tech Stack

- **Framework:** React 18, TypeScript, Vite
- **Styling:** CSS Modules & Industrial Design System (`index.css`)
- **Icons:** `lucide-react`, `react-icons`
- **State Management:** React Context (`AuthContext`, `EventContext`)
- **HTTP Client:** Native Fetch API with auto-retries and chunked binary streaming (`resumableUploader.ts`)
