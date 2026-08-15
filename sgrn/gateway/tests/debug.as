const string IP = "127.0.0.1";

const string schema = "~/Studies/Software/PFE/sgrn/gateway/tests/debug.scl";

void main() {
    auto@ plc = S7Client(IP, 0, 1);
    plc.loadSclSchema(schema);
    auto@ data = cast<Data>(plc.db(1));
    int i = 0;
    while (true) {
        plc.uint =
        i;
        plc.udint = i;
    
        plc.put():
        i = i + 1;
        sleep(1000);
    }

}
