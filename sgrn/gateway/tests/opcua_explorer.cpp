#include <iomanip>
#include <iostream>
#include <open62541/client_config_default.h>
#include <open62541/client_highlevel.h>

void browseRecursive(UA_Client* tp_client, UA_NodeId t_node_id, int t_level = 0) {
    if (t_level > 10)
        return; // Recursion depth limit

    UA_BrowseRequest b_req;
    UA_BrowseRequest_init(&b_req);
    b_req.requestedMaxReferencesPerNode = 0;
    b_req.nodesToBrowse = UA_BrowseDescription_new();
    b_req.nodesToBrowseSize = 1;
    UA_NodeId_copy(&t_node_id, &b_req.nodesToBrowse[0].nodeId);
    b_req.nodesToBrowse[0].resultMask = UA_BROWSERESULTMASK_ALL;

    UA_BrowseResponse b_resp = UA_Client_Service_browse(tp_client, b_req);
    if (b_resp.responseHeader.serviceResult != UA_STATUSCODE_GOOD) {
        UA_BrowseResponse_clear(&b_resp);
        UA_BrowseRequest_clear(&b_req);
        return;
    }

    for (size_t i = 0; i < b_resp.resultsSize; ++i) {
        if (b_resp.results[i].statusCode != UA_STATUSCODE_GOOD)
            continue;

        for (size_t j = 0; j < b_resp.results[i].referencesSize; ++j) {
            UA_ReferenceDescription* p_ref = &b_resp.results[i].references[j];

            // Skip standard NS0 nodes unless they are the root folder we want
            if (p_ref->nodeId.nodeId.namespaceIndex == 0 && t_level > 0)
                continue;

            std::string indent(t_level * 2, ' ');
            std::string name;
            if (p_ref->browseName.name.data) {
                name = std::string((char*)p_ref->browseName.name.data, p_ref->browseName.name.length);
            } else {
                name = "Unnamed";
            }

            if (p_ref->nodeClass == UA_NODECLASS_OBJECT) {
                std::cout << indent << "📁 " << name << std::endl;
                browseRecursive(tp_client, p_ref->nodeId.nodeId, t_level + 1);
            } else if (p_ref->nodeClass == UA_NODECLASS_VARIABLE) {
                // Try to read value
                UA_Variant value;
                UA_Variant_init(&value);
                UA_StatusCode read_val = UA_Client_readValueAttribute(tp_client, p_ref->nodeId.nodeId, &value);

                std::cout << indent << "🔹 " << name;
                if (read_val == UA_STATUSCODE_GOOD) {
                    if (UA_Variant_hasScalarType(&value, &UA_TYPES[UA_TYPES_DOUBLE])) {
                        std::cout << " = " << *(UA_Double*)value.data;
                    } else if (UA_Variant_hasScalarType(&value, &UA_TYPES[UA_TYPES_STRING])) {
                        UA_String* p_s = (UA_String*)value.data;
                        if (p_s->data) {
                            std::cout << " = " << std::string((char*)p_s->data, p_s->length);
                        }
                    }
                }
                std::cout << std::endl;
                UA_Variant_clear(&value);
            }
        }
    }
    UA_BrowseResponse_clear(&b_resp);
    UA_BrowseRequest_clear(&b_req);
}

int main(int t_argc, char* t_argv[]) {
    UA_Client* p_client = UA_Client_new();
    UA_ClientConfig_setDefault(UA_Client_getConfig(p_client));

    std::string url = (t_argc > 1) ? t_argv[1] : "opc.tcp://localhost:4840";
    std::cout << "🔍 Connecting to " << url << " (Native C++ Mode)..." << std::endl;

    UA_StatusCode retval = UA_Client_connect(p_client, url.c_str());
    if (retval != UA_STATUSCODE_GOOD) {
        std::cerr << "❌ Failed to connect: " << UA_StatusCode_name(retval) << std::endl;
        UA_Client_delete(p_client);
        return (int)retval;
    }

    std::cout << "✅ Connected! Exploring Digital Twin..." << std::endl;
    std::cout << "--------------------------------------------------" << std::endl;

    // Start browsing from the Objects folder
    browseRecursive(p_client, UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER));

    std::cout << "--------------------------------------------------" << std::endl;
    UA_Client_disconnect(p_client);
    UA_Client_delete(p_client);
    return 0;
}
