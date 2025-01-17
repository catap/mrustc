/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir_expand/static_borrow_constants.cpp
 * - Converts borrows of constant values into borrows of statics
 *
 * NOTE: This is done as a post-typeck HIR pass for the following reasons:
 * - Ensures that typecheck is performed on the as-written code
 * - Reduces load on MIR generation (no attempting to MIR lower large constant expressions)
 */
#include <hir/visitor.hpp>
#include <hir/expr.hpp>
#include <hir_typeck/static.hpp>
#include <hir_conv/constant_evaluation.hpp>
#include <algorithm>
#include "main_bindings.hpp"
#include <hir/expr_state.hpp>

namespace {
    inline HIR::ExprNodeP mk_exprnodep(HIR::ExprNode* en, ::HIR::TypeRef ty){ en->m_res_type = mv$(ty); return HIR::ExprNodeP(en); }
}
#define NEWNODE(TY, CLASS, ...)  mk_exprnodep(new HIR::ExprNode_##CLASS(__VA_ARGS__), TY)

namespace {

    class ExprVisitor_Mutate:
        public ::HIR::ExprVisitorDef
    {
    public:
        typedef std::function<HIR::SimplePath(Span, HIR::TypeRef, HIR::ExprPtr)>    t_new_static_cb;
    private:
        const StaticTraitResolve& m_resolve;
        t_new_static_cb m_new_static_cb;
        const ::HIR::ExprPtr& m_expr_ptr;

        bool    m_is_constant;
        bool    m_all_constant;
    public:
        ExprVisitor_Mutate(const StaticTraitResolve& resolve, t_new_static_cb new_static_cb, const ::HIR::ExprPtr& expr_ptr):
            m_resolve(resolve)
            ,m_new_static_cb( mv$(new_static_cb) )
            ,m_expr_ptr(expr_ptr)
            ,m_is_constant(false)
            ,m_all_constant(false)
        {
        }
        void visit_node_ptr(::HIR::ExprPtr& root) {
            const auto& node_ref = *root;
            const char* node_ty = typeid(node_ref).name();
            TRACE_FUNCTION_FR(&*root << " " << node_ty << " : " << root->m_res_type, node_ty);

            root->visit(*this);
        }
        void visit_node_ptr(::HIR::ExprNodeP& node) override {
            assert( node );
            const auto& node_ref = *node;
            const char* node_ty = typeid(node_ref).name();
            m_is_constant = false;
            {
                TRACE_FUNCTION_FR(&*node << " " << node_ty << " : " << node->m_res_type, node_ty << " " << m_is_constant);

                // If the inner didn't set `is_constant`, clear `all_constant`
                node->visit(*this);
                if( !m_is_constant ) {
                    m_all_constant = false;
                }
            }
            m_is_constant = false;
        }

        void visit(::HIR::ExprNode_Borrow& node) override {
            auto saved_all_constant = m_all_constant;
            m_all_constant = true;
            ::HIR::ExprVisitorDef::visit(node);
            // If the inner is constant (Array, Struct, Literal, const)
            if( m_all_constant )
            {
                // Handle a `_Unsize` inner
                auto* value_ptr_ptr = &node.m_value;
                if(auto* inner_node = dynamic_cast<::HIR::ExprNode_Unsize*>(value_ptr_ptr->get()))
                {
                    value_ptr_ptr = &inner_node->m_value;
                }
                auto& value_ptr = *value_ptr_ptr;

                // Not generic (can't check for interior mutability)
                if( monomorphise_type_needed(value_ptr->m_res_type) )
                {
                    DEBUG("-- " << value_ptr->m_res_type << " is generic");
                }
                // Not mutable (... or at least, not a non-shared non-zst)
                else if( node.m_type != HIR::BorrowType::Shared /*&& ([&]()->bool{ size_t v = 1; Target_GetSizeOf(value_ptr->span(), m_resolve, value_ptr->m_res_type, v); return v != 0; })()*/ )
                {
                    DEBUG("-- Mutable borrow of non-ZST");
                }
                // And it's not interior mutable
                else if( m_resolve.type_is_interior_mutable(value_ptr->span(), value_ptr->m_res_type) != HIR::Compare::Unequal )
                {
                    DEBUG("-- " << value_ptr->m_res_type << " could be interior mutable");
                }
                else
                {
                    DEBUG("-- Creating static");
                    auto val_expr = HIR::ExprPtr(mv$(value_ptr));
                    val_expr.m_state = ::HIR::ExprStatePtr(::HIR::ExprState(m_expr_ptr.m_state->m_module, m_expr_ptr.m_state->m_mod_path));
                    val_expr.m_state->m_traits = m_expr_ptr.m_state->m_traits;
                    val_expr.m_state->m_impl_generics = m_expr_ptr.m_state->m_impl_generics;
                    val_expr.m_state->m_item_generics = m_expr_ptr.m_state->m_item_generics;
                    val_expr.m_state->stage = ::HIR::ExprState::Stage::Typecheck;

                    // Create new static
                    auto sp = val_expr->span();
                    auto ty = val_expr->m_res_type.clone();
                    auto path = m_new_static_cb(sp, ty.clone(), mv$(val_expr));
                    DEBUG("> " << path);
                    // Update the `m_value` to point to a new node
                    auto new_node = NEWNODE(mv$(ty), PathValue, sp, mv$(path), HIR::ExprNode_PathValue::STATIC);
                    value_ptr = mv$(new_node);

                    m_is_constant = true;
                }
            }
            m_all_constant = saved_all_constant;
        }

        // - Composites (set local constant if all inner are constant)
        void visit(::HIR::ExprNode_ArraySized& node) override {
            ::HIR::ExprVisitorDef::visit(node);
            m_is_constant = m_all_constant;
        }
        void visit(::HIR::ExprNode_ArrayList& node) override {
            ::HIR::ExprVisitorDef::visit(node);
            m_is_constant = m_all_constant;
        }
        void visit(::HIR::ExprNode_StructLiteral& node) override {
            ::HIR::ExprVisitorDef::visit(node);
            m_is_constant = m_all_constant;
        }
        void visit(::HIR::ExprNode_TupleVariant& node) override {
            ::HIR::ExprVisitorDef::visit(node);
            m_is_constant = m_all_constant;
        }
        void visit(::HIR::ExprNode_Tuple& node) override {
            ::HIR::ExprVisitorDef::visit(node);
            m_is_constant = m_all_constant;
        }
        // - Accessors (constant if the inner is constant)
        void visit(::HIR::ExprNode_Field& node) override {
            ::HIR::ExprVisitorDef::visit(node);
            m_is_constant = m_all_constant;
        }
        // - Operations (only cast currently)
        void visit(::HIR::ExprNode_Cast& node) override {
            ::HIR::ExprVisitorDef::visit(node);
            m_is_constant = m_all_constant;
        }
        void visit(::HIR::ExprNode_Unsize& node) override {
            ::HIR::ExprVisitorDef::visit(node);
            m_is_constant = m_all_constant;
        }
        // - Root values
        void visit(::HIR::ExprNode_Literal& node) override {
            ::HIR::ExprVisitorDef::visit(node);
            m_is_constant = true;
        }
        void visit(::HIR::ExprNode_UnitVariant& node) override {
            ::HIR::ExprVisitorDef::visit(node);
            m_is_constant = true;
        }
        void visit(::HIR::ExprNode_PathValue& node) override {
            ::HIR::ExprVisitorDef::visit(node);
            MonomorphState  ms;
            // If the target is a constant, set `m_is_constant`
            auto v = m_resolve.get_value(node.span(), node.m_path, ms, /*signature_only*/true);
            switch(v.tag())
            {
            case StaticTraitResolve::ValuePtr::TAG_Constant:
            case StaticTraitResolve::ValuePtr::TAG_Function:
                if( monomorphise_path_needed(node.m_path) ) {
                    DEBUG("Constant path is still generic, can't transform into a `static`");
                }
                else {
                    m_is_constant = true;
                }
                break;
            default:
                break;
            }
        }
    };
    class OuterVisitor:
        public ::HIR::Visitor
    {
        const ::HIR::Crate& m_crate;
        StaticTraitResolve  m_resolve;

        const HIR::ItemPath*  m_current_module_path;
        const HIR::Module*  m_current_module;

        std::map<const HIR::Module*, std::vector< std::pair<HIR::SimplePath, HIR::Static> > >  m_new_statics;

    public:
        OuterVisitor(const ::HIR::Crate& crate)
            : m_crate(crate)
            , m_resolve(m_crate)
            , m_current_module(nullptr)
        {
        }

        ExprVisitor_Mutate::t_new_static_cb get_new_ty_cb()
        {
            return [this](Span sp, HIR::TypeRef ty, HIR::ExprPtr val_expr)->HIR::SimplePath {
                ASSERT_BUG(sp, m_current_module, "");
                // Assign a path (based on the current list)
                auto& list = m_new_statics[m_current_module];
                auto idx = list.size();
                auto name = RcString::new_interned( FMT("lifted#" << idx) );
                auto path = (*m_current_module_path + name).get_simple_path();
                auto new_static = HIR::Static(
                    HIR::Linkage(),
                    /*is_mut=*/false,
                    mv$(ty),
                    /*m_value=*/mv$(val_expr)
                    );
                DEBUG(path << " = " << new_static.m_value_res);
                list.push_back(std::make_pair( path, mv$(new_static) ));
                return path;
                };
        }

        void visit_crate(::HIR::Crate& crate) override {
            ::HIR::Visitor::visit_crate(crate);

            // Once the crate is complete, add the newly created statics to the module tree
            for(auto& mod_list : m_new_statics)
            {
                auto& mod = *const_cast<HIR::Module*>(mod_list.first);

                for(auto& new_static_pair : mod_list.second)
                {
                    struct NullNvs: ::HIR::Evaluator::Newval {
                        ::HIR::Path new_static(::HIR::TypeRef type, EncodedLiteral value) override { BUG(Span(), "Unexpected attempt to create a new value in extracted constant"); }
                    } null_nvs;
                    Span    sp;
                    auto& new_static = new_static_pair.second;
                    new_static.m_value_res = ::HIR::Evaluator(sp, m_crate, null_nvs).evaluate_constant( new_static_pair.first, new_static.m_value, new_static.m_type.clone());
                    new_static.m_value_generated = true;

                    mod.m_value_items.insert(std::make_pair( mv$(new_static_pair.first.m_components.back()), box$(HIR::VisEnt<HIR::ValueItem> {
                        HIR::Publicity::new_none(), // Should really be private, but we're well after checking
                        HIR::ValueItem(mv$(new_static_pair.second))
                        })) );
                }
            }
        }

        void visit_module(::HIR::ItemPath p, ::HIR::Module& mod) override {
            auto par = m_current_module;
            auto par_p = m_current_module_path;
            m_current_module = &mod;
            m_current_module_path = &p;

            ::HIR::Visitor::visit_module(p, mod);

            m_current_module = par;
            m_current_module_path = par_p;
        }
        void visit_type_impl(::HIR::TypeImpl& impl) override {
            DEBUG("impl " << impl.m_params.fmt_args() << " " << impl.m_type << " (from " << impl.m_src_module << ")");
            const auto& srcmod = m_crate.get_mod_by_path(Span(), impl.m_src_module);
            auto mod_ip = HIR::ItemPath(impl.m_src_module);
            m_current_module = &srcmod;
            m_current_module_path = &mod_ip;

            auto _ = m_resolve.set_impl_generics(impl.m_params);
            ::HIR::Visitor::visit_type_impl(impl);

            m_current_module = nullptr;
        }
        void visit_trait_impl(const ::HIR::SimplePath& trait_path, ::HIR::TraitImpl& impl) override {
            DEBUG("src module " << impl.m_src_module);
            const auto& srcmod = m_crate.get_mod_by_path(Span(), impl.m_src_module);
            auto mod_ip = HIR::ItemPath(impl.m_src_module);
            m_current_module = &srcmod;
            m_current_module_path = &mod_ip;

            auto _ = m_resolve.set_impl_generics(impl.m_params);
            ::HIR::Visitor::visit_trait_impl(trait_path, impl);

            m_current_module = nullptr;
        }

        // NOTE: This is left here to ensure that any expressions that aren't handled by higher code cause a failure
        void visit_expr(::HIR::ExprPtr& exp) override {
            BUG(Span(), "visit_expr hit in OuterVisitor");
        }

        void visit_type(::HIR::TypeRef& ty) override
        {
            if( auto* ep = ty.data_mut().opt_Array() )
            {
                this->visit_type( ep->inner );
                DEBUG("Array size " << ty);
                if( auto* cg = ep->size.opt_Unevaluated() ) {
                    if(cg->is_Unevaluated())
                    {
                        ExprVisitor_Mutate  ev(m_resolve, this->get_new_ty_cb(), *cg->as_Unevaluated());
                        ev.visit_node_ptr( *cg->as_Unevaluated() );
                    }
                }
            }
            else {
                ::HIR::Visitor::visit_type(ty);
            }
        }
        // ------
        // Code-containing items
        // ------
        void visit_function(::HIR::ItemPath p, ::HIR::Function& item) override {
            if( item.m_code )
            {
                auto _ = m_resolve.set_item_generics(item.m_params);
                DEBUG("Function code " << p);
                ExprVisitor_Mutate  ev(m_resolve, this->get_new_ty_cb(), item.m_code);
                ev.visit_node_ptr( item.m_code );
            }
            else
            {
                DEBUG("Function code " << p << " (none)");
            }
        }
        void visit_static(::HIR::ItemPath p, ::HIR::Static& item) override {
            if( item.m_value )
            {
                ExprVisitor_Mutate  ev(m_resolve, this->get_new_ty_cb(), item.m_value);
                ev.visit_node_ptr(item.m_value);
            }
        }
        void visit_constant(::HIR::ItemPath p, ::HIR::Constant& item) override {
            if( item.m_value )
            {
                ExprVisitor_Mutate  ev(m_resolve, this->get_new_ty_cb(), item.m_value);
                ev.visit_node_ptr(item.m_value);
            }
        }
        void visit_enum(::HIR::ItemPath p, ::HIR::Enum& item) override {
            if(auto* e = item.m_data.opt_Value())
            {
                auto _ = m_resolve.set_impl_generics(item.m_params);
                for(auto& var : e->variants)
                {
                    DEBUG("Enum value " << p << " - " << var.name);

                    if( var.expr )
                    {
                        ExprVisitor_Mutate  ev(m_resolve, this->get_new_ty_cb(), var.expr);
                        ev.visit_node_ptr(var.expr);
                    }
                }
            }
        }
    };
}   // namespace

void HIR_Expand_StaticBorrowConstants_Expr(const ::HIR::Crate& crate, ::HIR::ExprPtr& exp)
{
    StaticTraitResolve  resolve(crate);
    ExprVisitor_Mutate  ev(resolve, [&](Span sp, HIR::TypeRef ty, HIR::ExprPtr val)->HIR::SimplePath {
        // How will this work? Can't easily mutate the crate in this context
        // - 
        TODO(exp.span(), "Create new static in per-expression context");
        }, exp);
    ev.visit_node_ptr( exp );
}
void HIR_Expand_StaticBorrowConstants(::HIR::Crate& crate)
{
    OuterVisitor    ov(crate);
    ov.visit_crate( crate );
}
