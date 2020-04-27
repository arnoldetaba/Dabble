#include "node.h"

std::vector<std::shared_ptr<Scope>> scopeStack; // Each scope is a shared pointer so load and store operations can acess these scopes without owning the scope

llvm::LLVMContext context;
std::shared_ptr<llvm::Module> module = std::make_shared<llvm::Module>("top", context);
IRBuilder<> builder(context);


// Error Logging
static auto LogCodeGenError(const char *str) {
    std::cout <<"Code generation error: " << str << std::endl;
    exit(EXIT_FAILURE);
    return nullptr;
}

std::string GetValueTypeString(Value* V){
    std::string type_str;
    llvm::raw_string_ostream rso(type_str);

    V->getType()->print(rso);
    return rso.str();
}

/* Returns an LLVM type based on the identifier */
static Type *typeOf(const std::string& type) 
{
    if (type == "int" || type == "entier") {
        return Type::getInt32Ty(context);
    }
    else if (type == "double" || type == "reel") {
        return Type::getDoubleTy(context);
    }
    return Type::getVoidTy(context);
}


Value* IntegerNode::codegen(){
    return ConstantInt::get(Type::getInt32Ty(context), value, true);
}

Value* FloatNode::codegen(){
    return ConstantFP::get(Type::getDoubleTy(context), APFloat(value));
}

// only call when value of an identifier reference is to be loaded
Value* Identifier::codegen(){
    std::cout << "Accessing value of identifier: " << name << std::endl;
    if(scopeStack.back()->symbolTable.find(name) == scopeStack.back()->symbolTable.end())
        return LogCodeGenError("Unknown Identifier");
    return builder.CreateLoad(scopeStack.back()->symbolTable[name]);
}

Value* ArrayOperation::codegen(){
    /**
     * The getelementptr instruction is used to compute the addresses. It requires two operands; the first indexes through the pointer to the structure, and the
     * second indexes the index/field of the structure or array. The reason we need the first operand is because the getelementptr instr. does not do any memory
     * memory lookup when computing an address, and so cannot itself iterate through pointers. An array of structs or an array of arrays best illustrates this.
     * */
    std::cout << "Accessing value of array " << arr->name << std::endl;
    if(scopeStack.back()->symbolTable.find(arr->name) == scopeStack.back()->symbolTable.end())
            return LogCodeGenError("Acess to undefined array!!");
    // first calculate address with the getelementptr instruction
    auto arrAdd = scopeStack.back()->symbolTable[arr->name];
    auto addr = builder.CreateGEP(arrAdd, std::vector<Value*>{ConstantInt::get(Type::getInt32Ty(context), 0), index->codegen()});

    if(!rhsExpression){ // load operation ex tab@i
        return builder.CreateLoad(addr); // Then load from this address
    }
    // store operation ex tab @ i = 2;
    auto rvalue = rhsExpression->codegen();
   
    // for now we inserting with different type or size greater than array size will cause undefined behaviour.. In the future we'll create a separate symbol table
    // in each scope for arrays, which will hold their type and size. so we can check type and add append function, x@@ = i !!! Ain't that sweet.
    return builder.CreateStore(rvalue, addr);
}

Value* ArrayDeclaration::codegen(){
    std::cout << "Creating array: " << arrName->name << std::endl;
    ArrayType* arrTy = ArrayType::get(typeOf(arrType->name), arrSize);
    AllocaInst* alloc =  builder.CreateAlloca(arrTy, nullptr, "");
    scopeStack.back()->symbolTable[arrName->name] = alloc;

    if(initializerList.size()){ // generate code for initialization list
        std::cout << "Initializing " << arrName->name << std::endl;
        uint64_t idx = 0;
        for(auto& initElement : initializerList){
            builder.CreateStore(initElement->codegen(), builder.CreateGEP(alloc, std::vector<Value*>{ConstantInt::get(Type::getInt32Ty(context), 0),
                                                                                                    ConstantInt::get(Type::getInt32Ty(context), idx) }));
            idx++;
        }
    }
    return alloc;
}


Value* Assignment::codegen(){
    std::cout << "Creating assignment " << id->name << std::endl;
    // first check when variable has been declared
    if(scopeStack.back()->symbolTable.find(id->name) == scopeStack.back()->symbolTable.end())
        return LogCodeGenError("Undeclared variable");
    // now load it and assign its value
    Value* lvalue = scopeStack.back()->symbolTable[id->name];
    Value* rvalue = rhsExpression->codegen();

    if(rvalue->getType()->getPointerTo() !=  lvalue->getType()){ // if lvalue type isneq to rvalue type, then try to cast rvalue type to lvalue type
        if(lvalue->getType()->getPointerElementType()->isDoubleTy() && rvalue->getType()->isIntegerTy()) // cast int to double  
            rvalue = builder.CreateCast(Instruction::SIToFP, rvalue, Type::getDoubleTy(context));
        else if(lvalue->getType()->getPointerElementType()->isIntegerTy() && rvalue->getType()->isDoubleTy())
            rvalue = builder.CreateCast(Instruction::FPToSI, rvalue, Type::getInt32Ty(context));
        else
            return LogCodeGenError("Unknown Type in assignment");
        
    }
    return builder.CreateStore(rvalue, lvalue);
}


Value* VariableDeclaration::codegen(){
    std::cout << "Creating variable declaration " << type->name << " " << id->name << std::endl;
    AllocaInst *alloc =  builder.CreateAlloca(typeOf(type->name), 0, ""); // returns pointer to memory location of variable
    scopeStack.back()->symbolTable[id->name] = alloc; // we need to store that pointer
    if(assignmentExpr){ // generate code for initialization
        std::cout << "Initializing " << id->name << std::endl;
        std::unique_ptr<Assignment> assn = std::make_unique<Assignment>(std::move(id), std::move(assignmentExpr));
        assn->codegen();
    }
    return alloc;
}

Value* BinaryExpr::codegen(){
    /**
     * For +,-,/,* , if one of operands is a float then we cast all other non floats to floats and create a float BinOp.
     * For comparison operators, cast operands must be the same.
     * */
    bool  isLOperandFloat = false;
    Value *L = LHS->codegen();
    Value *R = RHS->codegen();
    if (!L || !R)
        return nullptr;
    
    if(L->getType()->isDoubleTy() || R->getType()->isDoubleTy()){
        L = builder.CreateCast(Instruction::SIToFP, L, Type::getDoubleTy(context));
        R = builder.CreateCast(Instruction::SIToFP, R, Type::getDoubleTy(context));
    }
    
    switch (Op) {
        case '+':
            if(L->getType()->isDoubleTy())
                return builder.CreateFAdd(L, R);
            return builder.CreateAdd(L, R);
        case '-':
            if(L->getType()->isDoubleTy())
                return builder.CreateFSub(L, R);
            return builder.CreateSub(L, R);
        case '*':
            if(L->getType()->isDoubleTy())
                return builder.CreateFMul(L, R);
            return builder.CreateMul(L, R);
        case '/':
            if(L->getType()->isDoubleTy())
                return builder.CreateFDiv(L, R);
            return builder.CreateSDiv(L, R, "", true);
        case '<':
            if(L->getType()->isDoubleTy())
                return builder.CreateFCmpOLT(L, R);
            return builder.CreateICmpSLT(L, R);
        case '>':
            if(L->getType()->isDoubleTy())
                return builder.CreateFCmpOGT(L, R);
            return builder.CreateICmpSGT(L, R);
        default:
            return LogCodeGenError("Unkown Binary Operator");
    }
}


Value* ReturnExpr::codegen(){
    std::cout << "Generating Return Stmt" << std::endl;
    Value* r = returnValue->codegen();
    if(!r) return nullptr;
    return builder.CreateRet(r);
}


Value* FunctionCallNode::codegen(){
    std::cout << "Creating function call to " << Callee->name << std::endl;
    std::vector<Value*> ArgsV; // holds codegen IR for each argument
    std::string StringFormat; // holds string formatting for all arguments
    if(Callee->name == "afficher"){
        for(auto& arg : Args){ // instead of doing this we can cast each argument to a string
            if(auto v = arg->codegen()){ 
                if(v->getType()->isDoubleTy())
                    StringFormat += "%.4f "; // we treat lire and afficher differently because of this
                else if(v->getType()->isIntegerTy())
                    StringFormat += "%d ";
                ArgsV.push_back(v);
            }else return nullptr;
        }
        StringFormat += "\n";
        ArgsV.insert(ArgsV.begin(), builder.CreateGlobalStringPtr(StringFormat));

        return builder.CreateCall(module->getFunction("printf"), ArgsV, "printfCall");
    }

    if(Callee->name == "lire"){
        for(auto& arg : Args){ // ar should always be an identifier.
            Identifier* id = dynamic_cast<Identifier*>(arg.get());
            if(auto v = arg->codegen()){ 
                if(v->getType()->isDoubleTy())
                    StringFormat += "%lf "; // 
                else if(v->getType()->isIntegerTy())
                    StringFormat += " %d";
                ArgsV.push_back(scopeStack.back()->symbolTable[id->name]);
            }else return nullptr;
        }
        ArgsV.insert(ArgsV.begin(), builder.CreateGlobalStringPtr(StringFormat));
        return builder.CreateCall(module->getFunction("scanf"), ArgsV, "scanfCall");
    }

    for(auto& arg : Args){
        if(auto v = arg->codegen())
            ArgsV.push_back(v);
        else return nullptr;
    }
    return builder.CreateCall(module->getFunction(Callee->name), ArgsV, "f_call");
}


Value* Prototype::codegen(){
    std::cout << "Creating Prototype for " << funcName->name << std::endl;
    std::vector<Type*> ArgTypes;
    for(auto& type: argTypes){
        Type* t = typeOf(type->name);
        ArgTypes.push_back(t);
    }
    llvm::FunctionType *FnType = llvm::FunctionType::get(typeOf(returnType->name), ArgTypes, false); 
    llvm::Function* fn = llvm::Function::Create(FnType, llvm::GlobalValue::ExternalLinkage, funcName->name, module.get());

    // Set names for all arguments so ir code is readable
    unsigned Idx = 0;
    for (auto &Arg : fn->args())
        Arg.setName(argNames[Idx++]->name);

    return fn;
}

Value* FunctionDefinition::codegen(){
    BasicBlock* PreFuncBlock = builder.GetInsertBlock();
    std::cout << "Creating Function definition" << std::endl;
    if(!prototype->codegen())
        return nullptr;

    Function* f = module->getFunction(prototype->funcName->name);
    BasicBlock *bb = BasicBlock::Create(context, "entry", f);
    scopeStack.push_back(std::make_shared<Scope>(bb));
    builder.SetInsertPoint(bb);

    for(auto &arg : f->args()){ // set the name->address correspondence in the local symboltable for each argument
        llvm::AllocaInst *alloca = builder.CreateAlloca(arg.getType(), 0, ""); // allocate memory on the stack to hold the arguments
        builder.CreateStore(static_cast<llvm::Value*>(&arg), alloca); // store each argument in each allocated memory space
        scopeStack.back()->symbolTable[std::string(arg.getName())] = alloca; // we use the names set in the protottype declaration i.e what the use set.
    }
    for(auto& s : Body){
        s->codegen();
    }
    verifyFunction(*f);
    scopeStack.pop_back();
    //builder.SetInsertPoint(scopeStack.back()->block);
    builder.SetInsertPoint(PreFuncBlock);
    return f;

}


Value* IfNode::codegen(){
    Value *CondV = Cond->codegen();
    if (!CondV)
        return nullptr;

    Function *currentFunction = builder.GetInsertBlock()->getParent();
    // Create blocks for the then and else cases. Insert the 'then' block at the
    // end of the function.
    BasicBlock *ThenBB = BasicBlock::Create(context, "then", currentFunction);
    BasicBlock *ElseBB = BasicBlock::Create(context, "else");
    BasicBlock *MergeBB = BasicBlock::Create(context, "ifcont");

    builder.CreateCondBr(CondV, ThenBB, ElseBB);

    // emit then block
    builder.SetInsertPoint(ThenBB);
    for(auto& s : Then)
        s->codegen();
    builder.CreateBr(MergeBB);

    // emit else block
    currentFunction->getBasicBlockList().push_back(ElseBB);// We do this so this block doesn't enter the function before the nested blocks of ThenBB
    builder.SetInsertPoint(ElseBB);
    // else if
    if(ElseIf){
        ElseIf->codegen();
    }
    // just else
    else if(!LoneElse.empty()){
        for(auto& s : LoneElse)
            s->codegen();
    }
    builder.CreateBr(MergeBB);
    // Emit merge block.
    currentFunction->getBasicBlockList().push_back(MergeBB);
    builder.SetInsertPoint(MergeBB);

    return static_cast<Value*>(nullptr); // need to change this so we can efficiently catch a codegen error
}

Value* WhileNode::codegen(){
    /**
     *                   |----> loop exit block   
     * condition block --|
     *      |            |----> loop body block-->|
     *      |-------------------------------------|
     * **/
    Function *currentFunction = builder.GetInsertBlock()->getParent();
    std::vector<Value*> BodyV; // holds codegen values for loop body
    
    BasicBlock *CondBB = BasicBlock::Create(context, "Condition", currentFunction);
    BasicBlock *LoopBB = BasicBlock::Create(context, "LoopBody");
    BasicBlock *ExitBB = BasicBlock::Create(context, "exitBody");
    // emit loop condition
    builder.CreateBr(CondBB);
    builder.SetInsertPoint(CondBB);
    Value *CondV = Cond->codegen();
    if (!CondV)
        return nullptr;
    
    builder.CreateCondBr(CondV, LoopBB, ExitBB);

    // emit loop body
    currentFunction->getBasicBlockList().push_back(LoopBB);
    builder.SetInsertPoint(LoopBB);
    for(auto& s : Body)
        s->codegen();

    builder.CreateBr(CondBB);

    // emit loop exit block
    currentFunction->getBasicBlockList().push_back(ExitBB);
    builder.SetInsertPoint(ExitBB); // The next block of instructions (which we don't know yet) will insert an unconditional branch here to itself.

    return static_cast<Value*>(nullptr);
}