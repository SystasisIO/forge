export module forge.contract.expression;

export import forge.expression.artifact;

export namespace forge::contract::expression {

[[nodiscard]] forge::expression::artifact compile(const forge::expression::compilation&);
[[nodiscard]] forge::expression::artifact load(const forge::expression::artifact&);
[[nodiscard]] forge::expression::result evaluate(const forge::expression::calculation&);

} // namespace forge::contract::expression
