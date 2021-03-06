/*********************************************************************
 *
 *  Software License Agreement
 *
 *  Copyright (c) 2020,
 *  TU Dortmund - Institute of Control Theory and Systems Engineering.
 *  All rights reserved.
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 *  Authors: Maximilian Krämer, Christoph Rösmann
 *********************************************************************/

#include <corbo-communication/utilities.h>
#include <corbo-optimal-control/structured_ocp/discretization_grids/finite_differences_grid_move_blocking.h>
#include <corbo-optimal-control/structured_ocp/edges/finite_differences_collocation_edges.h>

namespace corbo {

void FiniteDifferencesGridMoveBlocking::createEdges(NlpFunctions& nlp_fun, OptimizationEdgeSet& edges, SystemDynamicsInterface::Ptr dynamics)
{
    assert(isValid());

    // clear edges first
    // TODO(roesmann): we could implement a more efficient strategy without recreating the whole edgeset everytime
    edges.clear();

    int n = getN();

    std::vector<BaseEdge::Ptr> cost_terms, eq_terms, ineq_terms;
    int u_idx     = 0;
    int block_idx = -1;

    for (int k = 0; k < n - 1; ++k)
    {
        // update move blocking index
        if (k == u_idx)
        {
            ++block_idx;
            u_idx += _blocking_vector(block_idx);
        }

        VectorVertex& x_next = (k < n - 2) ? _x_seq[k + 1] : _xf;

        VectorVertex& u_prev  = (block_idx > 0) ? _u_seq[block_idx - 1] : _u_prev;
        ScalarVertex& dt_prev = (k > 0) ? _dt : _u_prev_dt;

        cost_terms.clear();
        eq_terms.clear();
        ineq_terms.clear();

        nlp_fun.getNonIntegralStageFunctionEdges(k, _x_seq[k], _u_seq[block_idx], _dt, u_prev, dt_prev, cost_terms, eq_terms, ineq_terms);
        for (BaseEdge::Ptr& edge : cost_terms) edges.addObjectiveEdge(edge);
        for (BaseEdge::Ptr& edge : eq_terms) edges.addEqualityEdge(edge);
        for (BaseEdge::Ptr& edge : ineq_terms) edges.addInequalityEdge(edge);

        if (nlp_fun.stage_cost && nlp_fun.stage_cost->hasIntegralTerms(k))
        {
            if (_cost_integration == CostIntegrationRule::TrapezoidalRule)
            {
                TrapezoidalIntegralCostEdge::Ptr edge =
                    std::make_shared<TrapezoidalIntegralCostEdge>(_x_seq[k], _u_seq[block_idx], x_next, _dt, nlp_fun.stage_cost, k);
                edges.addObjectiveEdge(edge);
            }
            else if (_cost_integration == CostIntegrationRule::LeftSum)
            {
                LeftSumCostEdge::Ptr edge = std::make_shared<LeftSumCostEdge>(_x_seq[k], _u_seq[block_idx], _dt, nlp_fun.stage_cost, k);
                edges.addObjectiveEdge(edge);
            }
            else
                PRINT_ERROR_NAMED("Cost integration rule not implemented");
        }

        if (nlp_fun.stage_equalities && nlp_fun.stage_equalities->hasIntegralTerms(k))
        {
            if (_cost_integration == CostIntegrationRule::TrapezoidalRule)
            {
                TrapezoidalIntegralEqualityDynamicsEdge::Ptr edge = std::make_shared<TrapezoidalIntegralEqualityDynamicsEdge>(
                    dynamics, _x_seq[k], _u_seq[block_idx], x_next, _dt, nlp_fun.stage_equalities, k);
                edge->setFiniteDifferencesCollocationMethod(_fd_eval);
                edges.addEqualityEdge(edge);
            }
            else if (_cost_integration == CostIntegrationRule::LeftSum)
            {
                LeftSumEqualityEdge::Ptr edge = std::make_shared<LeftSumEqualityEdge>(_x_seq[k], _u_seq[block_idx], _dt, nlp_fun.stage_equalities, k);
                edges.addEqualityEdge(edge);

                // system dynamics edge
                FDCollocationEdge::Ptr sys_edge = std::make_shared<FDCollocationEdge>(dynamics, _x_seq[k], _u_seq[block_idx], x_next, _dt);
                sys_edge->setFiniteDifferencesCollocationMethod(_fd_eval);
                edges.addEqualityEdge(sys_edge);
            }
            else
                PRINT_ERROR_NAMED("Cost integration rule not implemented");
        }
        else
        {
            // just the system dynamics edge
            FDCollocationEdge::Ptr edge = std::make_shared<FDCollocationEdge>(dynamics, _x_seq[k], _u_seq[block_idx], x_next, _dt);
            edge->setFiniteDifferencesCollocationMethod(_fd_eval);
            edges.addEqualityEdge(edge);
        }

        if (nlp_fun.stage_inequalities && nlp_fun.stage_inequalities->hasIntegralTerms(k))
        {
            if (_cost_integration == CostIntegrationRule::TrapezoidalRule)
            {
                TrapezoidalIntegralInequalityEdge::Ptr edge =
                    std::make_shared<TrapezoidalIntegralInequalityEdge>(_x_seq[k], _u_seq[block_idx], x_next, _dt, nlp_fun.stage_inequalities, k);
                edges.addInequalityEdge(edge);
            }
            else if (_cost_integration == CostIntegrationRule::LeftSum)
            {
                LeftSumInequalityEdge::Ptr edge =
                    std::make_shared<LeftSumInequalityEdge>(_x_seq[k], _u_seq[block_idx], _dt, nlp_fun.stage_inequalities, k);
                edges.addInequalityEdge(edge);
            }
            else
                PRINT_ERROR_NAMED("Cost integration rule not implemented");
        }
    }

    // check if we have a separate unfixed final state
    if (!_xf.isFixed())
    {
        // set final state cost
        BaseEdge::Ptr cost_edge = nlp_fun.getFinalStateCostEdge(n - 1, _xf);
        if (cost_edge) edges.addObjectiveEdge(cost_edge);

        // set final state constraint
        BaseEdge::Ptr constr_edge = nlp_fun.getFinalStateConstraintEdge(n - 1, _xf);
        if (constr_edge)
        {
            if (nlp_fun.final_stage_constraints->isEqualityConstraint())
                edges.addEqualityEdge(constr_edge);
            else
                edges.addInequalityEdge(constr_edge);
        }
    }

    // add control deviation edges for last control
    cost_terms.clear();
    eq_terms.clear();
    ineq_terms.clear();
    nlp_fun.getFinalControlDeviationEdges(n, _u_ref, _u_seq.back(), _dt, cost_terms, eq_terms, ineq_terms);
    for (BaseEdge::Ptr& edge : cost_terms) edges.addObjectiveEdge(edge);
    for (BaseEdge::Ptr& edge : eq_terms) edges.addEqualityEdge(edge);
    for (BaseEdge::Ptr& edge : ineq_terms) edges.addInequalityEdge(edge);
}

#ifdef MESSAGE_SUPPORT
void FiniteDifferencesGridMoveBlocking::fromMessage(const messages::FiniteDifferencesGridMoveBlocking& message, std::stringstream* issues)
{
    if (message.n() < 2 && issues) *issues << "FiniteDifferencesGridMoveBlocking: Number of states must be greater than or equal 2.\n";
    if (message.dt() <= 0 && issues) *issues << "FiniteDifferencesGridMoveBlocking: Dt must be greater than 0.0.\n";

    setNRef(message.n());
    setDtRef(message.dt());
    setWarmStart(message.warm_start());
    setWarmStartShiftU(message.warm_start_shift_u());

    switch (message.cost_integration_rule())
    {
        case messages::FiniteDifferencesGridMoveBlocking::CostIntegrationRule::FiniteDifferencesGridMoveBlocking_CostIntegrationRule_LeftSum:
        {
            setCostIntegrationRule(CostIntegrationRule::LeftSum);
            break;
        }
        case messages::FiniteDifferencesGridMoveBlocking::CostIntegrationRule::FiniteDifferencesGridMoveBlocking_CostIntegrationRule_TrapezoidalRule:
        {
            setCostIntegrationRule(CostIntegrationRule::TrapezoidalRule);
            break;
        }
        default:
        {
            PRINT_ERROR_NAMED("FiniteDifferencesGridMoveBlocking: Selected cost integration rule not implemented");
        }
    };

    // fd collocation method
    // construct object
    std::string type;
    util::get_oneof_field_type(message.fd_collocation(), "fd_collocation", type, false);
    FiniteDifferencesCollocationInterface::Ptr fd_eval = create_from_factory<FiniteDifferencesCollocationInterface>(type);
    // import parameters
    if (fd_eval)
    {
        fd_eval->fromMessage(message.fd_collocation(), issues);
        setFiniteDifferencesCollocationMethod(fd_eval);
    }
    else
    {
        if (issues) *issues << "FiniteDifferencesGridMoveBlocking: unknown finite differences collocation method specified.\n";
        return;
    }

    // Blocking vector
    if (message.b_size() > getNRef() - 1 && issues)
    {
        *issues << "FiniteDifferencesGridMoveBlocking: Cannot have more blocking indices than controls.\n";
        return;
    }
    _blocking_vector = Eigen::Map<const Eigen::VectorXi>(message.b().data(), message.b_size());
}

void FiniteDifferencesGridMoveBlocking::toMessage(messages::FiniteDifferencesGridMoveBlocking& message) const
{
    message.set_n(getNRef());
    message.set_dt(getDtRef());
    message.set_warm_start(_warm_start);
    message.set_warm_start_shift_u(_warm_start_shift_u);

    // fd collocation method
    if (_fd_eval) _fd_eval->fromMessage(*message.mutable_fd_collocation());

    switch (_cost_integration)
    {
        case CostIntegrationRule::LeftSum:
        {
            message.set_cost_integration_rule(
                messages::FiniteDifferencesGridMoveBlocking::CostIntegrationRule::FiniteDifferencesGridMoveBlocking_CostIntegrationRule_LeftSum);
            break;
        }
        case CostIntegrationRule::TrapezoidalRule:
        {
            message.set_cost_integration_rule(messages::FiniteDifferencesGridMoveBlocking::CostIntegrationRule::
                                                  FiniteDifferencesGridMoveBlocking_CostIntegrationRule_TrapezoidalRule);
            break;
        }
        default:
        {
            PRINT_ERROR_NAMED("FiniteDifferencesGridMoveBlocking: Selected cost integration rule not implemented");
        }
    };

    // Blocking vector
    message.mutable_b()->Resize(_blocking_vector.size(), 0);
    Eigen::Map<Eigen::VectorXi>(message.mutable_b()->mutable_data(), _blocking_vector.size()) = _blocking_vector;
}
#endif

}  // namespace corbo
