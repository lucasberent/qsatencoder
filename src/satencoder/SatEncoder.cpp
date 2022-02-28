#include "satencoder/SatEncoder.hpp"

bool SatEncoder::testEqual(qc::QuantumComputation& circuitOne, qc::QuantumComputation& circuitTwo, std::vector<std::string>& inputs, std::string& filename) {
    //std::cout << circuitOne << std::endl;
    //std::cout << circuitTwo << std::endl;

    if (!isClifford(circuitOne) || !isClifford(circuitTwo)) {
        std::cerr << "Circuits are not Clifford circuits" << std::endl;
        return false;
    }
    if (circuitOne.empty() || circuitTwo.empty()) {
        std::cerr << "Both circuits must be non-empy" << std::endl;
        return false;
    }
    stats.nrOfDiffInputStates                    = inputs.size();
    stats.nrOfQubits                             = circuitOne.getNqubits();
    qc::DAG                           dagOne     = qc::CircuitOptimizer::constructDAG(circuitOne);
    qc::DAG                           dagTwo     = qc::CircuitOptimizer::constructDAG(circuitTwo);
    SatEncoder::CircuitRepresentation circOneRep = preprocessCircuit(dagOne, inputs);
    SatEncoder::CircuitRepresentation circTwoRep = preprocessCircuit(dagTwo, inputs);
    std::cout << "Preprocessing complete - elapsed time (ms) for this task: " << stats.preprocTime << std::endl;

    z3::context ctx{};
    z3::solver  solver(ctx);
    constructMiterInstance(circOneRep, circTwoRep, solver);
    std::cout << "SAT construction complete - elapsed time (ms) for this task: " << stats.satConstructionTime << std::endl;

    bool equal  = !isSatisfiable(solver);
    stats.equal = equal;

    // for benchmarking
    if (!filename.empty()) {
        std::ofstream outfile(filename, std::fstream::app);
        outfile << "," << stats.to_json();
    }
    return equal;
}
void SatEncoder::checkSatisfiability(qc::QuantumComputation& circuitOne, std::vector<std::string>& inputs, std::string& filename) {
    if (!isClifford(circuitOne)) {
        std::cerr << "Circuit is not Clifford Circuit." << std::endl;
        return;
    }
    stats.nrOfDiffInputStates = inputs.size();
    stats.nrOfQubits          = circuitOne.getNqubits();
    qc::DAG dag               = qc::CircuitOptimizer::constructDAG(circuitOne);

    auto circRep = preprocessCircuit(dag, inputs);
    std::cout << "Preprocessing construction complete - elapsed time (ms) for this task: " << stats.preprocTime << std::endl;

    z3::context ctx{};
    z3::solver  solver(ctx);
    constructSatInstance(circRep, solver);
    std::cout << "SAT construction complete - elapsed time (ms) for this task: " << stats.satConstructionTime << std::endl;

    bool sat          = this->isSatisfiable(solver);
    stats.satisfiable = sat;
    // print to benchmark file
    if (!filename.empty()) {
        std::ofstream outfile(filename, std::fstream::app);
        outfile << "," << stats.to_json();
    }
}

bool SatEncoder::isSatisfiable(z3::solver& solver) {
    bool result            = false;
    std::cout << "Starting SAT solving" << std::endl;
    auto before            = std::chrono::high_resolution_clock::now();
    auto sat               = solver.check();
    auto after             = std::chrono::high_resolution_clock::now();
    auto z3SolvingDuration = std::chrono::duration_cast<std::chrono::milliseconds>(after - before).count();
    //std::cout << "Z3 solving complete - elapsed time (ms) for this task: " << z3SolvingDuration << std::endl;
    stats.solvingTime = z3SolvingDuration;
    if (sat == z3::check_result::sat) {
        //std::cout << "SATISFIABLE" << std::endl;
        stats.satisfiable = true;
        result            = true;
        //std::cout << "model " << solver.get_model() << std::endl;
    } else if (sat == z3::check_result::unsat) {
        //std::cout << "UNSATISFIABLE" << std::endl;
    } else {
        // std::cerr << "UNKNOWN" << std::endl;
    }
    //std::cout << solver.statistics() << std::endl;
    for (size_t i = 0; i < solver.statistics().size(); i++) {
        auto   key = solver.statistics().key(i);
        double val;
        if (solver.statistics().is_double(i)) {
            val = solver.statistics().double_value(i);
        } else {
            val = solver.statistics().uint_value(i);
        }
        stats.z3StatsMap.emplace(key, val);
    }
    return result;
}

SatEncoder::CircuitRepresentation SatEncoder::preprocessCircuit(qc::DAG& dag, std::vector<std::string>& inputs) {
    auto before = std::chrono::high_resolution_clock::now();
    //std::cout << "new ckt" << std::endl;
    unsigned long                     inputSize      = dag.size();
    unsigned long                     nrOfLevels     = 0;
    std::size_t                       nrOfOpsOnQubit = 0;
    unsigned long                     tmp;
    std::vector<QState>               states;
    SatEncoder::CircuitRepresentation representation;

    unsigned long nrOfQubits = dag.size();
    //compute nr of levels of ckt = #generators needed per input state
    for (std::size_t i = 0U; i < inputSize; i++) {
        tmp = dag.at(i).size();
        if (tmp > nrOfLevels) {
            nrOfLevels = tmp;
        }
    }
    stats.circuitDepth = nrOfLevels > stats.circuitDepth ? nrOfLevels : stats.circuitDepth;
    std::vector<std::map<boost::uuids::uuid, boost::uuids::uuid>> vec(nrOfLevels);
    representation.generatorMappings = vec;

    if (inputs.size() > 1) {
        for (auto& input: inputs) {
            states.push_back(initializeState(nrOfQubits, input));
        }
    } else {
        states.push_back(initializeState(nrOfQubits, {}));
    }

    //std::cout << "Init sttes" << std::endl;
    // store generators of input state
    for (auto& state: states) {
        auto               initLevelGenerator = state.getLevelGenerator();
        auto               inspair            = generators.emplace(initLevelGenerator, uniqueGenCnt); // put generator into global map if not already present
        boost::uuids::uuid genId{};
        if (inspair.second) { // if a new generator has been computed by this level (i.e., state changed)
            uniqueGenCnt++;
            genId = boost::uuids::random_generator()();
            generatorIdMap.emplace(initLevelGenerator, genId);
        } else {
            genId = generatorIdMap.at(initLevelGenerator);
        }
        representation.idGeneratorMap.emplace(genId, initLevelGenerator);
        state.SetPrevGenId(genId);
        //no generator <> generator mapping for initial level
    }

    if (nrOfInputGenerators == 0) { //only in first pass
        nrOfInputGenerators = uniqueGenCnt;
    }

    for (std::size_t levelCnt = 0; levelCnt < nrOfLevels; levelCnt++) {
        for (std::size_t qubitCnt = 0U; qubitCnt < inputSize; qubitCnt++) { //apply operation of current level for each qubit
            nrOfOpsOnQubit = dag.at(qubitCnt).size();

            if (levelCnt < nrOfOpsOnQubit) {
                if (!dag.at(qubitCnt).empty() && dag.at(qubitCnt).at(levelCnt) != nullptr) {
                    stats.nrOfGates++;
                    auto          gate    = dag.at(qubitCnt).at(levelCnt)->get();
                    unsigned long target  = gate->getTargets().at(0U);          //we assume we only have 1 target
                    unsigned long control = gate->getControls().begin()->qubit; //we assume we only have 1 control

                    //apply gate of level to each generator
                    for (auto& currState: states) {
                        if (gate->getType() == qc::OpType::H) {
                            currState.applyH(target);
                        } else if (gate->getType() == qc::OpType::S) {
                            currState.applyS(target);
                        } else if (gate->getType() == qc::OpType::Sdag) {
                            currState.applyS(target); // Sdag == SSS
                            currState.applyS(target);
                            currState.applyS(target);
                        } else if (gate->getType() == qc::OpType::Z) {
                            currState.applyH(target);
                            currState.applyS(target);
                            currState.applyS(target);
                            currState.applyH(target);
                        } else if (gate->getType() == qc::OpType::X && !gate->isControlled()) {
                            currState.applyH(target);
                            currState.applyS(target);
                            currState.applyS(target);
                        } else if (gate->getType() == qc::OpType::Y) {
                            currState.applyH(target);
                            currState.applyS(target);
                            currState.applyS(target);
                            currState.applyS(target);
                        } else if (gate->isControlled() && gate->getType() == qc::OpType::X) { //CNOT
                            if (qubitCnt == control) {                                         //CNOT is for control and target in DAG, only apply if current qubit is control
                                currState.applyCNOT(control, target);
                            }
                        } else {
                            // std::cerr << "unsupported operation" << std::endl;
                        }
                    }
                }
            }
        }
        for (auto& state: states) {
            auto               currLevelGen = state.getLevelGenerator();                      //extract generator representation from tableau (= list of paulis for each qubit)
            auto               inspair      = generators.emplace(currLevelGen, uniqueGenCnt); // put generator into global map if not already present
            boost::uuids::uuid genId{};
            if (inspair.second) { // new generator because newly inserted
                uniqueGenCnt++;
                genId = boost::uuids::random_generator()();
                generatorIdMap.emplace(currLevelGen, genId); // global generator <> id mapping for quick reverse lookup
            } else {                                         // generator already in global map
                genId = generatorIdMap.at(currLevelGen);
            }
            representation.idGeneratorMap.emplace(genId, currLevelGen);                                        // id <-> generator mapping
            representation.generatorMappings.at(levelCnt).insert(std::make_pair(state.GetPrevGenId(), genId)); // generator <> generator mapping at position level in list
            state.SetPrevGenId(genId);                                                                         // update previous generator id for next state
        }
    }
    auto after = std::chrono::high_resolution_clock::now();
    stats.preprocTime += std::chrono::duration_cast<std::chrono::milliseconds>(after - before).count();
    return representation;
}
//construct z3 instance from preprocessing information
void SatEncoder::constructSatInstance(SatEncoder::CircuitRepresentation& circuitRepresentation, z3::solver& solver) {
    auto before = std::chrono::high_resolution_clock::now();
    // number of unique generators that need to be encoded
    const auto generatorCnt = generators.size();
    stats.nrOfGenerators    = generatorCnt;

    // bitwidth required to encode the generators
    const auto bitwidth = static_cast<std::size_t>(std::ceil(std::log2(generatorCnt)));

    // whether the number of generators is a power of two or not
    bool blockingConstraintsNeeded = std::log2(generatorCnt) < static_cast<double>(bitwidth);

    // z3 context used throughout this function
    auto& ctx = solver.ctx();

    const auto        depth = circuitRepresentation.generatorMappings.size();
    std::vector<z3::expr> vars{};
    vars.reserve(depth + 1U);
    std::string bvName = "x^";

    for (std::size_t k = 0U; k <= depth; k++) {
        // create bitvector [x^k]_2 with respective bitwidth for each level k of ckt
        std::stringstream ss{};
        ss << bvName << k; //
        vars.emplace_back(ctx.bv_const(ss.str().c_str(), bitwidth));
        stats.nrOfSatVars++;
    }

    for (std::size_t i = 0U; i < depth; i++) {
        const auto layer = circuitRepresentation.generatorMappings.at(i); // generator<>generator map for level i
        for (const auto& [from, to]: layer) {
            const auto g1 = generators.at(circuitRepresentation.idGeneratorMap.at(from));
            const auto g2 = generators.at(circuitRepresentation.idGeneratorMap.at(to));

            // create [x^l]_2 = i => [x^l']_2 = k for each generator mapping
            const auto left  = vars[i] == ctx.bv_val(static_cast<std::uint64_t>(g1), bitwidth);
            const auto right = vars[i + 1U] == ctx.bv_val(static_cast<std::uint64_t>(g2), bitwidth);
            const auto cons  = implies(left, right);
            solver.add(cons);
            stats.nrOfFunctionalConstr++;
        }
    }

    if (blockingConstraintsNeeded) {
        for (const auto& var: vars) {
            const auto cons = ult(var, ctx.bv_val(static_cast<std::uint64_t>(generatorCnt), bitwidth));
            solver.add(cons); // [x^l]_2 < m
        }
    }
    auto after                = std::chrono::high_resolution_clock::now();
    stats.satConstructionTime = std::chrono::duration_cast<std::chrono::milliseconds>(after - before).count();
}

void SatEncoder::constructMiterInstance(SatEncoder::CircuitRepresentation& circOneRep, SatEncoder::CircuitRepresentation& circTwoRep, z3::solver& solver) {
    auto before = std::chrono::high_resolution_clock::now();
    // number of unique generators that need to be encoded
    const auto generatorCnt = generators.size();
    stats.nrOfGenerators    = generatorCnt;
    // bitwidth required to encode the generators
    const auto bitwidth = static_cast<std::size_t>(std::ceil(std::log2(generatorCnt)));

    // whether the number of generators is a power of two or not
    bool blockingConstraintsNeeded = std::log2(generatorCnt) < static_cast<double>(bitwidth);
    // z3 context used throughout this function
    auto& ctx = solver.ctx();

    /// encode first circuit
    const auto        depthOne = circOneRep.generatorMappings.size();
    std::vector<z3::expr> varsOne{};
    varsOne.reserve(depthOne + 1U);
    std::string bvName = "x^";

    //std::cout << "Vars1" << std::endl;
    for (std::size_t k = 0U; k <= depthOne; k++) {
        // create bitvector [x^k]_2 with respective bitwidth for each level k of ckt
        std::stringstream ss{};
        ss << bvName << k; //
        const auto tmp = ctx.bv_const(ss.str().c_str(), bitwidth);
        varsOne.emplace_back(tmp);
        //std::cout << tmp << std::endl;
        //solver.add(ule(ctx.bv_val(static_cast<std::uint64_t>(0), bitwidth) ,tmp));
        stats.nrOfSatVars++;
    }

    //std::cout << "Func " << std::endl;
    for (std::size_t i = 0U; i < depthOne; i++) {
        const auto layer = circOneRep.generatorMappings.at(i); // generator<>generator map for level i
        for (const auto& [from, to]: layer) {
            const auto g1 = generators.at(circOneRep.idGeneratorMap.at(from));
            const auto g2 = generators.at(circOneRep.idGeneratorMap.at(to));

            // create [x^l]_2 = i <=> [x^l']_2 = k for each generator mapping
            const auto left  = varsOne[i] == ctx.bv_val(static_cast<std::uint64_t>(g1), bitwidth);
            const auto right = varsOne[i + 1U] == ctx.bv_val(static_cast<std::uint64_t>(g2), bitwidth);
            const auto cons  = (left == right);
            solver.add(cons);
            //std::cout << cons << std::endl;
            stats.nrOfFunctionalConstr++;
        }
    }

    //std::cout << "Blocking" << std::endl;
    if (blockingConstraintsNeeded) {
        for (const auto& var: varsOne) {
            const auto cons = ult(var, ctx.bv_val(static_cast<std::uint64_t>(generatorCnt), bitwidth));
            solver.add(cons); // [x^l]_2 < m
            //std::cout << cons << std::endl;
        }
    }
    /// encode second circuit
    auto              depthTwo = circTwoRep.generatorMappings.size();
    std::vector<z3::expr> varsTwo{};
    varsOne.reserve(depthTwo + 1U);
    bvName = "x'^";

    //std::cout << "Vars2" << std::endl;
    for (std::size_t k = 0U; k <= depthTwo; k++) {
        // create bitvector [x^k]_2 with respective bitwidth for each level k of ckt
        std::stringstream ss{};
        ss << bvName << k; //
        const auto tmp = ctx.bv_const(ss.str().c_str(), bitwidth);
        varsTwo.emplace_back(tmp);
        //solver.add(ule(ctx.bv_val(static_cast<std::uint64_t>(0), bitwidth), tmp));
        stats.nrOfSatVars++;
        //std::cout << tmp << std::endl;
    }

    //std::cout << "Func2" << std::endl;
    for (std::size_t i = 0U; i < depthTwo; i++) {
        const auto layer = circTwoRep.generatorMappings.at(i); // generator<>generator map for level i
        for (const auto& [from, to]: layer) {
            const auto g1 = generators.at(circTwoRep.idGeneratorMap.at(from));
            const auto g2 = generators.at(circTwoRep.idGeneratorMap.at(to));

            // create [x^l]_2 = i <=> [x^l']_2 = k for each generator mapping
            const auto left  = varsTwo[i] == ctx.bv_val(static_cast<std::uint64_t>(g1), bitwidth);
            const auto right = varsTwo[i + 1U] == ctx.bv_val(static_cast<std::uint64_t>(g2), bitwidth);
            const auto cons  = (left == right);
            solver.add(cons);
            //std::cout << cons << std::endl;
            stats.nrOfFunctionalConstr++;
        }
    }

    //std::cout << "Blocking2 " << std::endl;
    if (blockingConstraintsNeeded) {
        for (const auto& var: varsTwo) {
            const auto cons = ult(var, ctx.bv_val(static_cast<std::uint64_t>(generatorCnt), bitwidth));
            solver.add(cons); // [x^l]_2 < m
            //std::cout << cons << std::endl;
        }
    }
    // create miter structure
    // if initial signals are the same, then the final signals have to be equal as well
    const auto equalInputs    = varsOne.front() == varsTwo.front();
    const auto unequalOutputs = varsOne.back() != varsTwo.back();
    const auto nrOfInputs     = ctx.bv_val(static_cast<std::uint64_t>(nrOfInputGenerators), bitwidth);
    const auto input1         = ult(varsOne.front(), nrOfInputs);
    const auto input2         = ult(varsTwo.front(), nrOfInputs);

    //std::cout << "miter " << std::endl;
    //std::cout << equalInputs << std::endl;
    //std::cout << unequalOutputs << std::endl;
    //std::cout << input1 << std::endl;
    //d::cout << input2 << std::endl;
    solver.add(equalInputs);
    solver.add(unequalOutputs);
    solver.add(input1);
    solver.add(input2);
    auto after                = std::chrono::high_resolution_clock::now();
    stats.satConstructionTime = std::chrono::duration_cast<std::chrono::milliseconds>(after - before).count();
}

bool SatEncoder::isClifford(qc::QuantumComputation& qc) {
    qc::OpType opType;
    for (const auto& op: qc) {
        opType = op->getType();
        if (opType != qc::OpType::H &&
            opType != qc::OpType::S &&
            opType != qc::OpType::Sdag &&
            opType != qc::OpType::X &&
            opType != qc::OpType::Z &&
            opType != qc::OpType::Y &&
            opType != qc::OpType::I) {
            return false;
        }
    }
    return true;
}
std::vector<boost::dynamic_bitset<>> SatEncoder::QState::getLevelGenerator() const {
    std::size_t                          size = (2U * n) + 1U;
    std::vector<boost::dynamic_bitset<>> result{};

    for (std::size_t i = 0U; i < n; i++) {
        boost::dynamic_bitset<> gen(size);
        for (std::size_t j = 0U; j < n; j++) {
            gen[j] = x.at(i)[j];
        }
        for (std::size_t j = 0; j < n; j++) {
            gen[n + j] = z.at(i)[j];
        }
        if (r.at(i) == 1) {
            gen[n + n] = true;
        } else {
            gen[n + n] = false; //either 0 or 1 possible for phase
        }
        result.emplace_back(gen);
    }

    return result;
}
SatEncoder::QState SatEncoder::initializeState(unsigned long nrOfQubits, std::string input) {
    SatEncoder::QState result;
    result.SetN(nrOfQubits);
    std::vector<boost::dynamic_bitset<>> tx(nrOfQubits);
    std::vector<boost::dynamic_bitset<>> tz(nrOfQubits);
    std::vector<int>                     tr(nrOfQubits, 0);

    for (std::size_t i = 0U; i < nrOfQubits; i++) {
        tx[i] = boost::dynamic_bitset(nrOfQubits);
        tz[i] = boost::dynamic_bitset(nrOfQubits);
        for (std::size_t j = 0U; j < nrOfQubits; j++) {
            if (i == j) {
                tz[i][j] = true; // initial 0..0 state corresponds to x matrix all zero and z matrix = Id_n
            }
        }
    }

    result.SetX(tx);
    result.SetZ(tz);
    result.SetR(tr);
    if (!input.empty()) { //
        for (std::size_t i = 0U; i < input.length(); i++) {
            switch (input[i]) {
                case 'Z': // stab by -Z = |1>
                    result.applyH(i);
                    result.applyS(i);
                    result.applyS(i);
                    result.applyH(i);
                    break;
                case 'x': // stab by X = |+>
                    result.applyH(i);
                    break;
                case 'X': // stab by -X = |->
                    result.applyH(i);
                    result.applyS(i);
                    result.applyS(i);
                    break;
                case 'y': // stab by Y = |0> + i|1>
                    result.applyH(i);
                    result.applyS(i);
                    break;
                case 'Y': // stab by -Y = |0> - i|1>
                    result.applyH(i);
                    result.applyS(i);
                    result.applyS(i);
                    result.applyS(i);
                    break;
            }
        }
    }
    return result;
}

void SatEncoder::QState::applyCNOT(unsigned long control, unsigned long target) {
    if (target > n || control > n) {
        return;
    }
    for (std::size_t i = 0U; i < n; ++i) {
        r[i] ^= (x[i][control] * z[i][target]) * (x[i][target] ^ z[i][control] ^ 1);
        x[i][target] ^= x[i][control];
        z[i][control] ^= z[i][target];
    }
}
void SatEncoder::QState::applyH(unsigned long target) {
    if (target > n) {
        return;
    }
    for (std::size_t i = 0U; i < n; i++) {
        r[i] ^= x[i][target] * z[i][target];
        x[i][target] ^= z[i][target];
        z[i][target] = x[i][target] ^ z[i][target];
        x[i][target] ^= z[i][target];
    }
}
void SatEncoder::QState::applyS(unsigned long target) {
    if (target > n) {
        return;
    }
    for (std::size_t i = 0U; i < n; ++i) {
        r[i] ^= x[i][target] * z[i][target];
        z[i][target] ^= x[i][target];
    }
}
void SatEncoder::QState::SetN(unsigned long N) {
    QState::n = N;
}
const std::vector<boost::dynamic_bitset<>>& SatEncoder::QState::GetX() const {
    return x;
}
void SatEncoder::QState::SetX(const std::vector<boost::dynamic_bitset<>>& X) {
    QState::x = X;
}
void SatEncoder::QState::SetZ(const std::vector<boost::dynamic_bitset<>>& Z) {
    QState::z = Z;
}
void SatEncoder::QState::SetR(const std::vector<int>& R) {
    QState::r = R;
}
const boost::uuids::uuid& SatEncoder::QState::GetPrevGenId() const {
    return prevGenId;
}
void SatEncoder::QState::SetPrevGenId(const boost::uuids::uuid& prev_gen_id) {
    prevGenId = prev_gen_id;
}
void SatEncoder::QState::printStateTableau() {
    std::cout << std::endl;
    for (std::size_t i = 0U; i < n; i++) {
        for (std::size_t j = 0U; j < n; j++) {
            std::cout << x.at(i)[j];
        }
        std::cout << "|";
        for (std::size_t j = 0U; j < n; j++) {
            std::cout << z.at(i)[j];
        }
        std::cout << "|";
        std::cout << r.at(i);
        std::cout << std::endl;
    }
    std::cout << std::endl;
}
json SatEncoder::Statistics::to_json() {
    return json{
            {"numGates", nrOfGates},
            {"nrOfQubits", nrOfQubits},
            {"numSatVarsCreated", nrOfSatVars},
            {"numGenerators", nrOfGenerators},
            {"numFuncConstr", nrOfFunctionalConstr},
            {"circDepth", circuitDepth},
            {"numInputs", nrOfDiffInputStates},
            {"equivalent", equal},
            {"satisfiable", satisfiable},
            {"preprocTime", preprocTime},
            {"solvingTime", solvingTime},
            {"satConstructionTime", satConstructionTime},
            {"z3map", z3StatsMap}

    };
}
void SatEncoder::Statistics::from_json(const json& j) {
    j.at("numGates").get_to(nrOfGates);
    j.at("nrOfQubits").get_to(nrOfQubits);
    j.at("numSatVarsCreated").get_to(nrOfSatVars);
    j.at("numGenerators").get_to(nrOfGenerators);
    j.at("numFuncConstr").get_to(nrOfFunctionalConstr);
    j.at("circDepth").get_to(circuitDepth);
    j.at("numInputs").get_to(nrOfDiffInputStates);
    j.at("equivalent").get_to(equal);
    j.at("satisfiable").get_to(satisfiable);
    j.at("preprocTime").get_to(preprocTime);
    j.at("solvingTime").get_to(solvingTime);
    j.at("satConstructionTime").get_to(satConstructionTime);
    j.at("z3map").get_to(z3StatsMap);
}