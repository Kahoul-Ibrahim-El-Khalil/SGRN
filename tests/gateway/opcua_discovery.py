import asyncio
import sys
import enum

# --- MONKEY PATCH START ---
# Fix for asyncua + Python 3.14 'issubclass' bug
try:
    import asyncua.ua.ua_binary as ua_binary
    _original_create_type_serializer = ua_binary.create_type_serializer

    def _patched_create_type_serializer(uatype):
        # If uatype is not a class, it can't be a subclass of Enum
        if not isinstance(uatype, type):
            # We return a dummy check or handle basic types
            # Most non-class types in this context are typing constructs
            return _original_create_type_serializer(uatype)
        return _original_create_type_serializer(uatype)

    # We need to wrap the issubclass call itself if possible, 
    # but patching the serializer factory is safer.
    import inspect
    
    # A more aggressive patch to avoid the TypeError globally in this module
    original_issubclass = issubclass
    def safe_issubclass(cls, classinfo):
        if not isinstance(cls, type):
            return False
        return original_issubclass(cls, classinfo)
    
    # Inject safe_issubclass into the problematic module's namespace
    ua_binary.issubclass = safe_issubclass
    
except Exception as e:
    print(f"⚠️ Warning: Could not apply Python 3.14 patch: {e}")

from asyncua import Client, ua
# --- MONKEY PATCH END ---

async def main():
    url = "opc.tcp://127.0.0.1:4840"
    print(f"🔍 Connecting to {url} (Python 3.14 Patched Mode)...")
    
    client = Client(url=url)
    try:
        await client.connect()
        print("✅ Connected successfully!")
        
        objects = client.get_objects_node()
        print("\nListing top-level objects:")
        
        # Get children of Objects
        children = await objects.get_children()
        for child in children:
            bn = await child.read_browse_name()
            # Skip standard server nodes for brevity
            if bn.Name in ["Server"]: continue
            
            print(f"  📁 {bn.Name}")
            
            # Explore Digital Twin DBs
            vars = await child.get_children()
            for v in vars:
                v_bn = await v.read_browse_name()
                try:
                    val = await v.read_value()
                    print(f"    🔹 {v_bn.Name} = {val}")
                except:
                    print(f"    🔹 {v_bn.Name}")
                    
    except Exception as e:
        print(f"❌ Error: {e}")
        import traceback
        traceback.print_exc()
    finally:
        try:
            await client.disconnect()
        except:
            pass

if __name__ == "__main__":
    asyncio.run(main())
