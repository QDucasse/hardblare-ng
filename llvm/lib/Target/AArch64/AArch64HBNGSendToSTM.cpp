#include "AArch64.h"
#include "AArch64InstrInfo.h"
#include "AArch64MachineFunctionInfo.h"
#include "AArch64Subtarget.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
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
#define TARGET_REG AArch64::X26

namespace {
class AArch64HBNGSendToSTM : public MachineFunctionPass {
    const AArch64InstrInfo *TII;
    const TargetRegisterInfo *TRI;
public:
    static char ID;
    AArch64HBNGSendToSTM() : MachineFunctionPass(ID) {
        initializeAArch64HBNGSendToSTMPass(*PassRegistry::getPassRegistry());
    }

    // Insert a register value store to STM for loads/stores
    void insertStoreForAddressOperand(MachineBasicBlock &MBB, MachineInstr &MI);

    // Main entrypoint for LLVM backend passes
    bool runOnMachineFunction(MachineFunction &MF) override;

    StringRef getPassName() const override {
        return "AArch64 HBNG Send To STM";
    }

private:
    unsigned normalizeRegister(unsigned Reg);
};

char AArch64HBNGSendToSTM::ID = 0;

} // end anonymous namespace

// Pass initialization
INITIALIZE_PASS(AArch64HBNGSendToSTM, DEBUG_TYPE, "HardBlare-NG Send to STM pass", false, false)


//===----------------------------------------------------------------------===//
// Helpers

static inline bool isPreOrPostIndexed(unsigned Opcode) {
    switch (Opcode) {
        // Loads
        case AArch64::LDRBBpre: case AArch64::LDRBBpost: case AArch64::LDRBpre: case AArch64::LDRBpost:
        case AArch64::LDRHHpre: case AArch64::LDRHHpost: case AArch64::LDRHpre: case AArch64::LDRHpost:
        case AArch64::LDRWpre: case AArch64::LDRWpost: case AArch64::LDRSpre: case AArch64::LDRSpost:
        case AArch64::LDRXpre: case AArch64::LDRXpost: case AArch64::LDRDpre: case AArch64::LDRDpost:
        // Stores
        case AArch64::STRBBpre: case AArch64::STRBBpost: case AArch64::STRBpre: case AArch64::STRBpost:
        case AArch64::STRHHpre: case AArch64::STRHHpost: case AArch64::STRHpre: case AArch64::STRHpost:
        case AArch64::STRWpost: case AArch64::STRWpre: case AArch64::STRSpost: case AArch64::STRSpre:
        case AArch64::STRXpost: case AArch64::STRXpre: case AArch64::STRDpost: case AArch64::STRDpre:
        case AArch64::STRQpost: case AArch64::STRQpre:
        // Pairs
        case AArch64::LDPWpre: case AArch64::LDPWpost: case AArch64::LDPDpre: case AArch64::LDPDpost:
        case AArch64::LDPXpre: case AArch64::LDPXpost: case AArch64::LDPSpre: case AArch64::LDPSpost:
        case AArch64::LDPQpre: case AArch64::LDPQpost:

        case AArch64::STPXpre: case AArch64::STPXpost: case AArch64::STPDpre: case AArch64::STPDpost:
        case AArch64::STPWpre: case AArch64::STPWpost: case AArch64::STPSpre: case AArch64::STPSpost:
        case AArch64::STPQpre: case AArch64::STPQpost:

            return true;
        default:
            return false;
    }
}


// TODO:
// case AArch64::STLURXi:
// case AArch64::STLURWi:
// case AArch64::STLURHi:
// case AArch64::STLURBi:

// case AArch64::LDAPURXi:
// case AArch64::LDAPURi:
// case AArch64::LDAPURSWi:
// case AArch64::LDAPURHi:
// case AArch64::LDAPURSHWi:
// case AArch64::LDAPURSHXi:
// case AArch64::LDAPURBi:
// case AArch64::LDAPURSBWi:
// case AArch64::LDAPURSBXi:

// case AArch64::LDNPQi:
// case AArch64::LDNPXi:
// case AArch64::LDNPDi:
// case AArch64::LDNPWi:
// case AArch64::LDNPSi:

// case AArch64::STNPQi:
// case AArch64::STNPXi:
// case AArch64::STNPDi:
// case AArch64::STNPWi:
// case AArch64::STNPSi:





//===----------------------------------------------------------------------===//
// Main insertion function

void AArch64HBNGSendToSTM::insertStoreForAddressOperand(MachineBasicBlock &MBB, MachineInstr &MI) {
    // Reserved register holding the STM fixed address
    unsigned TargetReg = TARGET_REG;

    unsigned int Opcode = MI.getOpcode();
    StringRef Mnemonic = TII->getName(Opcode);

    unsigned BaseRegister;
    unsigned OffsetRegister;
    unsigned int BaseRegisterIndex;

    const TargetRegisterClass *RC1;
    const TargetRegisterClass *RC2;

    LLVM_DEBUG(dbgs() << "Setting STM send for instruction: " << Mnemonic << "\n");

    switch(Opcode) {
        default:
            LLVM_DEBUG(dbgs() << "Unsupported instruction" << Mnemonic << "\n");
            break;

        // Base register, immediate offset (maybe post/pre)
        // STR/LDR X1 [X2]     -> Send X2      (STR)
        // Scaled
        case AArch64::LDRBBui: case AArch64::LDRBui: case AArch64::LDRSBXui:
        case AArch64::LDRHHui: case AArch64::LDRHui: case AArch64::LDRSHWui: case AArch64::LDRSHXui:
        case AArch64::LDRWui: case AArch64::LDRSWui: case AArch64::LDRSui: case AArch64::LDRSBWui:
        case AArch64::LDRXui: case AArch64::LDRDui:
        case AArch64::LDRQui:
        // Unscaled
        case AArch64::LDURQi:
        case AArch64::LDURXi: case AArch64::LDURDi: case AArch64::LDURSHXi:
        case AArch64::LDURWi: case AArch64::LDURSWi: case AArch64::LDURSi:
        case AArch64::LDURHi: case AArch64::LDURHHi: case AArch64::LDURSHWi:
        case AArch64::LDURBi: case AArch64::LDURBBi: case AArch64::LDURSBWi: case AArch64::LDURSBXi:
        // Pre/post index
        case AArch64::LDRBBpre: case AArch64::LDRBBpost: case AArch64::LDRBpre: case AArch64::LDRBpost:
        case AArch64::LDRHHpre: case AArch64::LDRHHpost: case AArch64::LDRHpre: case AArch64::LDRHpost:
        case AArch64::LDRWpre: case AArch64::LDRWpost: case AArch64::LDRSpre: case AArch64::LDRSpost:
        case AArch64::LDRXpre: case AArch64::LDRXpost: case AArch64::LDRDpre: case AArch64::LDRDpost:
        case AArch64::LDRQpre: case AArch64::LDRQpost:
        // Scaled
        case AArch64::STRBui: case AArch64::STRBBui: case AArch64::STURBBi:
        case AArch64::STRHui: case AArch64::STRHHui:
        case AArch64::STRWui: case AArch64::STRSui:
        case AArch64::STRXui: case AArch64::STRDui:
        case AArch64::STRQui:
        // Unscaled
        case AArch64::STURBi:
        case AArch64::STURHi: case AArch64::STURHHi:
        case AArch64::STURWi: case AArch64::STURSi:
        case AArch64::STURXi: case AArch64::STURDi:
        case AArch64::STURQi:
        // Pre/post index
        case AArch64::STRBBpre: case AArch64::STRBBpost: case AArch64::STRBpre: case AArch64::STRBpost:
        case AArch64::STRHHpre: case AArch64::STRHHpost: case AArch64::STRHpre: case AArch64::STRHpost:
        case AArch64::STRWpost: case AArch64::STRWpre: case AArch64::STRSpost: case AArch64::STRSpre:
        case AArch64::STRXpost: case AArch64::STRXpre: case AArch64::STRDpost: case AArch64::STRDpre:
        case AArch64::STRQpost: case AArch64::STRQpre:
            // Store the value of the base register into [Target Reg]
            BaseRegisterIndex = isPreOrPostIndexed(Opcode) ? 2 : 1;
            BaseRegister = MI.getOperand(BaseRegisterIndex).getReg();
            // The stack pointer cannot be stored directly and sent to the STM
            // Instead, it is expected that it is sent at the start of the tracing
            // process.
            if (BaseRegister == AArch64::SP) break;
            // STR BaseReg [TargetReg]
            BuildMI(MBB, MI, MI.getDebugLoc(), TII->get(AArch64::STRXui))
                .addReg(BaseRegister)
                .addReg(TargetReg)
                .addImm(0);
            break;

        // Base register, register offset (maybe shifted/extended)
        // STR/LDR X1 [X2, X3] -> Send X2, X3  (STP)
        // loads
        case AArch64::LDRBBroW: case AArch64::LDRBroW:
        case AArch64::LDRBBroX: case AArch64::LDRBroX:
        case AArch64::LDRHHroW: case AArch64::LDRHroW:
        case AArch64::LDRHHroX: case AArch64::LDRHroX:
        case AArch64::LDRWroW:  case AArch64::LDRSWroW: case AArch64::LDRSroW:
        case AArch64::LDRWroX: case AArch64::LDRSWroX: case AArch64::LDRSroX:
        case AArch64::LDRXroW: case AArch64::LDRDroW:
        case AArch64::LDRXroX: case AArch64::LDRDroX:
        case AArch64::LDRSHXroX: case AArch64::LDRSHXroW: case AArch64::LDRSHWroX: case AArch64::LDRSHWroW:
        case AArch64::LDRSBXroX: case AArch64::LDRSBXroW: case AArch64::LDRSBWroX: case AArch64::LDRSBWroW:
        case AArch64::LDRQroX: case AArch64::LDRQroW:
        // Stores
        case AArch64::STRBBroW: case AArch64::STRBroW:
        case AArch64::STRBBroX: case AArch64::STRBroX:
        case AArch64::STRHHroW: case AArch64::STRHroW:
        case AArch64::STRHHroX: case AArch64::STRHroX:
        case AArch64::STRWroW: case AArch64::STRSroW:
        case AArch64::STRWroX: case AArch64::STRSroX:
        case AArch64::STRXroW: case AArch64::STRDroW:
        case AArch64::STRXroX: case AArch64::STRDroX:
        case AArch64::STRQroX: case AArch64::STRQroW:
            // Store the value of the base register and the register offset into [Target Reg]
            BaseRegister = MI.getOperand(1).getReg();
            OffsetRegister = MI.getOperand(2).getReg();
            // The stack pointer cannot be stored directly and sent to the STM
            // Instead, it is expected that it is sent at the start of the tracing
            // process.
            if (BaseRegister == AArch64::SP) break;

            // FIXME: If the offset and base register are not of the same size, STP is not a valid
            // instruction. For now, emit two store instructions (would be nice to upscale the
            // smallest register).
            RC1 = TRI->getMinimalPhysRegClass(BaseRegister);
            RC2 = TRI->getMinimalPhysRegClass(OffsetRegister);
            if (RC1 != RC2) {
                BuildMI(MBB, MI, MI.getDebugLoc(), TII->get(AArch64::STRXui))
                    .addReg(BaseRegister)
                    .addReg(TargetReg)
                    .addImm(0);
                BuildMI(MBB, MI, MI.getDebugLoc(), TII->get(AArch64::STRXui))
                    .addReg(OffsetRegister)
                    .addReg(TargetReg)
                    .addImm(0);
            }
            else {
                // STP OffsetReg, BaseReg,  [TargetReg]
                // /!\ WARNING they need to be reversed to appear in the STM as 1. base reg 2. offset reg
                BuildMI(MBB, MI, MI.getDebugLoc(), TII->get(AArch64::STPXi))
                    .addReg(OffsetRegister)
                    .addReg(BaseRegister)
                    .addReg(TargetReg)
                    .addImm(0);
            }
            break;

        // Base register, pair load (maybe shifted/extended)
        // STP/LDP X1, X2 [X3] -> Send X3      (STR)
        case AArch64::LDPWi: case AArch64::LDPSWi: case AArch64::LDPSi:
        case AArch64::LDPXi: case AArch64::LDPDi:
        case AArch64::LDPQi:
        case AArch64::STPWi: case AArch64::STPSi:
        case AArch64::STPXi: case AArch64::STPDi:
        case AArch64::STPQi:
        case AArch64::LDPWpre: case AArch64::LDPWpost: case AArch64::LDPSpre: case AArch64::LDPSpost:
        case AArch64::LDPXpre: case AArch64::LDPXpost: case AArch64::LDPDpre: case AArch64::LDPDpost:
        case AArch64::LDPQpre: case AArch64::LDPQpost:
        case AArch64::STPWpre: case AArch64::STPWpost: case AArch64::STPSpre: case AArch64::STPSpost:
        case AArch64::STPXpre: case AArch64::STPXpost: case AArch64::STPDpre: case AArch64::STPDpost:
        case AArch64::STPQpre: case AArch64::STPQpost:
            // Store the value of the base register and the register offset into [Target Reg]
            BaseRegisterIndex = isPreOrPostIndexed(Opcode) ? 3 : 2;
            BaseRegister = MI.getOperand(BaseRegisterIndex).getReg();
            // The stack pointer cannot be stored directly and sent to the STM
            // Instead, it is expected that it is sent at the start of the tracing
            // process.
            if (BaseRegister == AArch64::SP) break;
            // STR BaseReg [TargetReg]
            BuildMI(MBB, MI, MI.getDebugLoc(), TII->get(AArch64::STRXui))
                .addReg(BaseRegister)
                .addReg(TargetReg)
                .addImm(0);
            break;
    }
}

//===----------------------------------------------------------------------===//
// Main loop

bool AArch64HBNGSendToSTM::runOnMachineFunction(MachineFunction &MF) {
    LLVM_DEBUG(dbgs() << "***** HBNG Send To STM *****\n");

    // Early bailout if the function has attribute hbng_no_instr
    const Function &F = MF.getFunction();
    if (F.hasFnAttribute("hbng_no_instr")) {
      LLVM_DEBUG(dbgs() << "Skipping instrumentation for " << F.getName() << "\n");
      return false;
    }


    // Initialize subtarget information as attributes
    auto &STI = MF.getSubtarget<AArch64Subtarget>();
    TII = STI.getInstrInfo();
    TRI = MF.getSubtarget().getRegisterInfo();

    bool Changed = false;

    LLVM_DEBUG(dbgs() << "func: " << MF.getName() << "\n");
    for (auto &MBB : MF) {
        LLVM_DEBUG(dbgs() << "block: " << MF.getName() << "\n");
        for (auto MI = MBB.begin(); MI != MBB.end(); ++MI) {
            // Only process stores/loads
            if (!(MI->mayLoad() || MI->mayStore())) {
                continue;
            }

            // Add the store to STM
            insertStoreForAddressOperand(MBB, *MI);
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
