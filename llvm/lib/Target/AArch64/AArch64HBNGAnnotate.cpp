#include "AArch64.h"
#include "AArch64MachineFunctionInfo.h"
#include "AArch64Subtarget.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

#define DEBUG_TYPE "hbng-annotate"

//===----------------------------------------------------------------------===//
// Helper functions


//===----------------------------------------------------------------------===//
// Main class definition

namespace {
class AArch64HBNGAnnotate : public MachineFunctionPass {
    const TargetInstrInfo *TII;
    std::vector<MDNode *> Annotations;

public:
    static char ID;
    AArch64HBNGAnnotate() : MachineFunctionPass(ID) {
      initializeAArch64HBNGAnnotatePass(*PassRegistry::getPassRegistry());
    }

    // Generate annotations for all instructions in a given machine function
    void generateAnnotation(MachineInstr &MI);

    // Main entrypoint for LLVM backend passes
    bool runOnMachineFunction(MachineFunction &MF) override;

    StringRef getPassName() const override {
      return "AArch64 HBNG Annotate";
    }

private:
    void storeAnnotation(const StringRef &funcName, const StringRef &mnemonic, LLVMContext &Ctx);
};

char AArch64HBNGAnnotate::ID = 0;

} // end anonymous namespace

// Pass initialization
INITIALIZE_PASS(AArch64HBNGAnnotate, DEBUG_TYPE, "HardBlare-NG annotation pass", false, false)


//===----------------------------------------------------------------------===//
// Annotation generation functions


void AArch64HBNGAnnotate::storeAnnotation(const StringRef &funcName, const StringRef &mnemonic, LLVMContext &Ctx) {
  Metadata *OpData[] = {
      MDString::get(Ctx, funcName),  // Function Name
      MDString::get(Ctx, mnemonic)   // Instruction Mnemonic
  };
  Annotations.push_back(MDTuple::get(Ctx, OpData)); // Store metadata in vector
}

//===----------------------------------------------------------------------===//
// Main loop

bool AArch64HBNGAnnotate::runOnMachineFunction(MachineFunction &MF) {
    LLVM_DEBUG(dbgs() << "***** HBNG Annotations *****\n");
    // Initialize subtarget information
    auto &STI = MF.getSubtarget<AArch64Subtarget>();
    TII = STI.getInstrInfo();
    // Get the context and module
    Module *M = MF.getFunction().getParent();
    LLVMContext &Ctx = MF.getFunction().getContext();

    // Attach collected data as module metadata, avoids polluting global symbols
    // and will be extracted at the emission stage and added to the binary.
    NamedMDNode *NMD = M->getOrInsertNamedMetadata("hbng_annotation_info");

    LLVM_DEBUG(dbgs() << "func: " << MF.getName() << "\n");
    for (auto &MBB : MF) {
      LLVM_DEBUG(dbgs() << "bblock: " << MBB.getName() << "\n");
      for (auto &MI : MBB) {
        generateAnnotation(MI);
        unsigned int opcode = MI.getOpcode();
        StringRef mnemonic = TII->getName(opcode);
        LLVM_DEBUG(dbgs() << "instr: " << mnemonic << "\n");

        // Store function name and instruction mnemonic
        storeAnnotation(MF.getName(), mnemonic, Ctx);
      }
    }

    for (auto &Annotation : Annotations) {
      NMD->addOperand(Annotation);
    }

    // Notify if the content has changed
    return false;
}


// Factory function used by AArch64TargetMachine to add the pass to
// the passmanager.
FunctionPass *llvm::createAArch64HBNGAnnotatePass() {
  return new AArch64HBNGAnnotate();
}