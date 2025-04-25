#include "AArch64.h"
#include "AArch64InstrInfo.h"
#include "AArch64MachineFunctionInfo.h"
#include "AArch64Subtarget.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineOperand.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/MC/MCContext.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "MCTargetDesc/AArch64AddressingModes.h"
#include "MCTargetDesc/AArch64MCTargetDesc.h"



using namespace llvm;

#define DEBUG_TYPE "hbng-send-stm"

namespace {
    class AArch64HBNGSendToSTM : public MachineFunctionPass {
    public:
        static char ID;

        AArch64HBNGSendToSTM() : MachineFunctionPass(ID) {
            initializeAArch64HBNGSendToSTMPass(*PassRegistry::getPassRegistry());
        }

        // Main entrypoint for LLVM backend passes
        bool runOnMachineFunction(MachineFunction &MF) override;

        StringRef getPassName() const override {
          return "AArch64 HBNG Send To STM";
        }

    };

    char AArch64HBNGSendToSTM::ID = 0;

} // end anonymous namespace

// Pass initialization
INITIALIZE_PASS(AArch64HBNGSendToSTM, DEBUG_TYPE, "HardBlare-NG Send to STM pass", false, false)


//===----------------------------------------------------------------------===//
// Main loop

bool AArch64HBNGSendToSTM::runOnMachineFunction(MachineFunction &MF) {
    LLVM_DEBUG(dbgs() << "***** HBNG Send To STM *****\n");
    // Initialize subtarget information as attributes
    auto &STI = MF.getSubtarget<AArch64Subtarget>();
    auto *TII = STI.getInstrInfo();

    bool Changed = false;

    for (auto &MBB : MF) {
      for (auto MI = MBB.begin(); MI != MBB.end(); ) {
            // Get the machine instruction
            MachineInstr &Instr = *MI++;

            if (!(Instr.mayLoad() || Instr.mayStore())) {
                continue;
            }
        DebugLoc DL = Instr.getDebugLoc();

        // FIXME: Possible instructions, put into separate functions
        // STR/LDR X1 [X2]     -> Send X2      (STR)
        // STR/LDR X1 [X2, X3] -> Send X2, X3  (STP)
        // STP/LDP X1, X2 [X3] -> Send X3      (STR)
        // Pairs cannot have a register offset

        BuildMI(MBB, MI, DL, TII->get(AArch64::STRXui))
            .addReg(AArch64::X0)         // FIXME: value to store, the base register of the next instruction
            .addReg(AArch64::X15)        // base register (must be reserved!), see AArch64.h
            .addImm(0);                    // offset (0 bytes)

        Changed = true;
      }
    }

    return Changed;
}


// Factory function used by AArch64TargetMachine to add the pass to
// the passmanager.
FunctionPass *llvm::createAArch64HBNGSendToSTMPass() {
    return new AArch64HBNGSendToSTM();
}
