process.env.NODE_TLS_REJECT_UNAUTHORIZED = "0";

const SERVER_URL = (
  process.env.SGRN_SERVER_URL || "http://localhost:5000"
).replace(/\/$/, "");
const ADMIN_EMAIL = process.env.SGRN_ADMIN_EMAIL || "admin@local.com";
const ADMIN_PASSWORD = process.env.SGRN_ADMIN_PASSWORD || "adminroot";

async function runAdminTests() {
  console.log("==========================================");
  console.log("SGRN Datastore Admin Features & Resumable API Test");
  console.log(`Server: ${SERVER_URL}`);
  console.log("==========================================");

  let token: string | null = null;

  // 1. Sign in as Admin User
  console.log("\n[1/5] Signing in as Admin...");
  try {
    const res = await fetch(`${SERVER_URL}/api/v1/auth/user/signin`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ email: ADMIN_EMAIL, password: ADMIN_PASSWORD }),
    });

    if (res.ok) {
      const data = await res.json();
      token = data.token;
      console.log(
        `✓ Admin sign-in successful (token: ${token?.slice(0, 10)}...)`,
      );
    } else {
      console.warn(
        `! Admin sign-in returned HTTP ${res.status}. Proceeding with bearer token if set.`,
      );
      token = process.env.SGRN_BEARER_TOKEN || null;
    }
  } catch (err: any) {
    console.warn(`! Connection warning during sign-in: ${err.message}`);
    token = process.env.SGRN_BEARER_TOKEN || null;
  }

  const authHeaders: Record<string, string> = token
    ? { Authorization: `Bearer ${token}` }
    : {};

  // 2. Test Live Active Sessions (Metaprobe)
  console.log(
    "\n[2/5] Testing Live Active Sessions (GET /api/v1/admin/metaprobe/sessions)...",
  );
  try {
    const res = await fetch(`${SERVER_URL}/api/v1/admin/metaprobe/sessions`, {
      method: "GET",
      headers: { ...authHeaders },
    });

    if (res.ok) {
      const sessions = await res.json();
      console.log(
        `✓ Live Active Sessions fetched successfully: ${Array.isArray(sessions) ? sessions.length : 0} active session(s)`,
      );
      if (Array.isArray(sessions) && sessions.length > 0) {
        console.log(
          `   Sample session actor: ${sessions[0].actor_name} (${sessions[0].actor_type})`,
        );
      }
    } else {
      console.error(`✗ Metaprobe sessions failed with HTTP ${res.status}`);
    }
  } catch (err: any) {
    console.error(`✗ Metaprobe sessions test error: ${err.message}`);
  }

  // 3. Test Webhooks API (List, Register, Delete)
  console.log("\n[3/5] Testing Webhooks API...");
  let registeredWebhookId: number | null = null;
  try {
    // List existing webhooks
    const listRes = await fetch(`${SERVER_URL}/api/v1/admin/webhooks`, {
      method: "GET",
      headers: { ...authHeaders },
    });
    if (listRes.ok) {
      const webhooks = await listRes.json();
      console.log(
        `✓ Listed webhooks: ${Array.isArray(webhooks) ? webhooks.length : 0} webhook(s) found`,
      );
    }

    // Register a test webhook
    const testWebhookUrl = "https://example.com/sgrn-test-webhook";
    const regRes = await fetch(`${SERVER_URL}/api/v1/admin/webhooks`, {
      method: "POST",
      headers: { "Content-Type": "application/json", ...authHeaders },
      body: JSON.stringify({
        url: testWebhookUrl,
        secret: "test-secret-key-123",
      }),
    });

    if (regRes.status === 201 || regRes.ok) {
      const data = await regRes.json();
      registeredWebhookId = data.id;
      console.log(
        `✓ Registered test webhook successfully (ID: ${registeredWebhookId}, URL: ${testWebhookUrl})`,
      );
    } else {
      console.error(`✗ Webhook registration failed with HTTP ${regRes.status}`);
    }

    // Delete the test webhook if created
    if (registeredWebhookId !== null) {
      const delRes = await fetch(
        `${SERVER_URL}/api/v1/admin/webhooks/${registeredWebhookId}`,
        {
          method: "DELETE",
          headers: { ...authHeaders },
        },
      );
      if (delRes.ok) {
        console.log(
          `✓ Deleted test webhook successfully (ID: ${registeredWebhookId})`,
        );
      } else {
        console.error(`✗ Webhook deletion failed with HTTP ${delRes.status}`);
      }
    }
  } catch (err: any) {
    console.error(`✗ Webhooks API test error: ${err.message}`);
  }

  // 4. Test Resumable Chunked Upload API
  console.log("\n[4/5] Testing Resumable Chunked Upload API...");
  try {
    const testFileName = `test_resumable_${Date.now()}.bin`;
    const dummyChunkSize = 1024 * 1024; // 1MB chunk
    const totalSize = dummyChunkSize * 2; // 2MB total file size

    // Init upload session
    const initRes = await fetch(`${SERVER_URL}/api/v1/storage/upload/init`, {
      method: "POST",
      headers: { "Content-Type": "application/json", ...authHeaders },
      body: JSON.stringify({
        filename: testFileName,
        target_path: "/",
        total_size: totalSize,
        chunk_size: dummyChunkSize,
      }),
    });

    if (initRes.ok) {
      const initData = await initRes.json();
      const uploadId = initData.upload_id;
      console.log(
        `✓ Initialized upload session (upload_id: ${uploadId}, total_chunks: ${initData.total_chunks})`,
      );

      // Upload Chunk 0
      const chunkData = new Uint8Array(dummyChunkSize).fill(65); // 'A'
      const chunkRes = await fetch(
        `${SERVER_URL}/api/v1/storage/upload/chunk?upload_id=${uploadId}&chunk_index=0`,
        {
          method: "PUT",
          headers: {
            "Content-Type": "application/octet-stream",
            ...authHeaders,
          },
          body: chunkData,
        },
      );

      if (chunkRes.ok) {
        console.log(`✓ Chunk 0 uploaded successfully`);
      } else {
        console.error(
          `✗ Uploading chunk 0 failed with HTTP ${chunkRes.status}`,
        );
      }

      // Check upload status
      const statusRes = await fetch(
        `${SERVER_URL}/api/v1/storage/upload/status?upload_id=${uploadId}`,
        {
          method: "GET",
          headers: { ...authHeaders },
        },
      );

      if (statusRes.ok) {
        const statusData = await statusRes.json();
        console.log(
          `✓ Upload status retrieved: ${statusData.uploaded_chunks_count}/${statusData.total_chunks} chunks uploaded`,
        );
      }

      // Upload Chunk 1
      const chunkRes1 = await fetch(
        `${SERVER_URL}/api/v1/storage/upload/chunk?upload_id=${uploadId}&chunk_index=1`,
        {
          method: "PUT",
          headers: {
            "Content-Type": "application/octet-stream",
            ...authHeaders,
          },
          body: chunkData,
        },
      );
      if (chunkRes1.ok) {
        console.log(`✓ Chunk 1 uploaded successfully`);
      }

      // Complete upload session
      const completeRes = await fetch(
        `${SERVER_URL}/api/v1/storage/upload/complete`,
        {
          method: "POST",
          headers: { "Content-Type": "application/json", ...authHeaders },
          body: JSON.stringify({ upload_id: uploadId }),
        },
      );

      if (completeRes.ok) {
        console.log(
          `✓ Completed upload session successfully (${testFileName})`,
        );
      } else {
        console.error(
          `✗ Completing upload session failed with HTTP ${completeRes.status}`,
        );
      }
    } else {
      console.error(
        `✗ Initializing upload session failed with HTTP ${initRes.status}`,
      );
    }
  } catch (err: any) {
    console.error(`✗ Resumable upload test error: ${err.message}`);
  }

  // 5. Test Resumable Download (HTTP Range Request)
  console.log("\n[5/5] Testing Resumable Download (Range: bytes=0-100)...");
  try {
    const dlRes = await fetch(`${SERVER_URL}/api/v1/storage/files?path=/`, {
      method: "GET",
      headers: { Range: "bytes=0-100", ...authHeaders },
    });

    if (dlRes.status === 206 || dlRes.ok) {
      console.log(
        `✓ Range download request returned status ${dlRes.status} (Content-Range: ${dlRes.headers.get("content-range") || "n/a"})`,
      );
    } else {
      console.log(`! Download request returned HTTP ${dlRes.status}`);
    }
  } catch (err: any) {
    console.error(`✗ Resumable download test error: ${err.message}`);
  }

  console.log("\n==========================================");
  console.log("Admin Features & Resumable API Test Completed.");
  console.log("==========================================");
}

runAdminTests().catch((e) => {
  console.error("Test execution failed:", e);
  process.exit(1);
});
