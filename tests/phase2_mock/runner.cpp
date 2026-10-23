#include "Arduino.h"
#include "../../firmware/mega-can/src/phase2/main.cpp"
#include <iostream>
int main(int argc,char **argv) {
    mock::fault=argc>1?argv[1]:"healthy";
    try { setup(); } catch(const Halt&) {}
    const std::string output=Serial.out.str();
    const bool pass=output.find("PHASE 2: PASS")!=std::string::npos;
    const bool fail=output.find("PHASE 2: FAIL")!=std::string::npos;
    if(pass==fail || pass!=(mock::fault=="healthy")) {
        std::cerr<<output; return 1;
    }
    if(pass && (output.find("Frames verified: 1000")==std::string::npos ||
                output.find("Frames transmitted (TX complete): 1000")==std::string::npos)) return 2;
    if(mock::fault=="missing" && output.find("Missing frames: 1")==std::string::npos) return 3;
    if(mock::fault=="duplicate" && output.find("Duplicate frames: 1")==std::string::npos) return 4;
    if(mock::fault=="tx_pending" && output.find("Timeouts: 1")==std::string::npos) return 5;
    if(mock::fault!="stop_fail") {
        for(int i=0;i<2;++i)
            if(!mock::absent(i) && mock::regs[i][0x0E]!=0x80) return 6;
    }
    if(argc>2) { std::cout<<output; return 0; }
    std::cout<<"HOST MOCK PASS "<<mock::fault<<" (not a hardware result)\n";
}
