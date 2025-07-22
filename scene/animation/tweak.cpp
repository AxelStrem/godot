/**************************************************************************/
/*  tweak.cpp                                                             */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "tweak.h"

#include "scene/main/node.h"
#include "core/variant/variant.h"
#include "core/object/object.h"

Tweak::Tweak() {
	ERR_FAIL_MSG("Tweak can't be created directly. Use create_tweak() method.");
}

Tweak::Tweak(Object* p_object, const StringName& p_property, const Variant &p_value, Tweak::ActionType action, int _priority) {
	if(p_object == nullptr)
	{
		return;
	}

	ObjectTweaker* p_obj_tweaker = p_object->get_object_tweaker();
	if(p_obj_tweaker == nullptr)
	{
		p_obj_tweaker = memnew(ObjectTweaker);
		p_object->set_object_tweaker(p_obj_tweaker);
	}

	PropertyTweaker* p_prop_tweaker = p_obj_tweaker->get_property_tweaker(p_property);
	if(p_prop_tweaker == nullptr)
	{
		return;
	}

	switch (action)
	{
	case ACTION_ADD:  pImpl = memnew(TweakAdd); break;
	case ACTION_SUBTRACT:  pImpl = memnew(TweakSubtract); break;
	case ACTION_MULTIPLY:  pImpl = memnew(TweakMultiply); break;
	case ACTION_DIVIDE:  pImpl = memnew(TweakDivide); break;
	case ACTION_AND: pImpl = memnew(TweakLogicalAnd); break;
	case ACTION_OR: pImpl = memnew(TweakLogicalOr); break;
	case ACTION_SET:
	default: pImpl = memnew(TweakImpl); break;
	}

	pImpl->priority = _priority;
	pImpl->tweak_value = p_value;

	p_prop_tweaker->add_tweak(pImpl);

	valid = true;
}

Tweak::Tweak(Object *p_object, const StringName &p_property, Object *p_source, const StringName &p_source_property, Tweak::ActionType action, int priority) :
	Tweak(p_object, p_property, Variant{}, action, priority) {
		if(valid)
		{
			ObjectTweaker* p_obj_tweaker = p_source->get_object_tweaker();
			if(p_obj_tweaker == nullptr)
			{
				p_obj_tweaker = memnew(ObjectTweaker);
				p_source->set_object_tweaker(p_obj_tweaker);
			}

			PropertyTweaker* p_prop_tweaker = p_obj_tweaker->get_property_tweaker(p_source_property);
			if(p_prop_tweaker == nullptr)
			{
				valid = false;
				return;
			}

			TweakMonitor* pMonitor = memnew(TweakMonitor);
			pMonitor->set_priority(INT32_MAX);
			pMonitor->set_observer(pImpl);
			p_prop_tweaker->add_tweak(pMonitor);

			pImpl->set_monitor(pMonitor);
		}
}

void Tweak::set_owning_tweaker(PropertyTweaker *p_tweaker) {
	pImpl->p_owner = p_tweaker;
}

void Tweak::set_order(int _order) {
	pImpl->order = _order;
}

void Tweak::set_value(const Variant& new_value) {
	pImpl->set_value(new_value);
}


Tweak::~Tweak() {
	if(pImpl)
	{
		memdelete(pImpl);
	}
}

void Tweak::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_value", "new_value"), &Tweak::set_value);
	ClassDB::bind_method(D_METHOD("get_value"), &Tweak::get_value);

	ADD_PROPERTY(PropertyInfo(Variant::NIL, "value", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NIL_IS_VARIANT), "set_value", "get_value");

	BIND_ENUM_CONSTANT(ACTION_ADD);
	BIND_ENUM_CONSTANT(ACTION_SUBTRACT);
	BIND_ENUM_CONSTANT(ACTION_MULTIPLY);
	BIND_ENUM_CONSTANT(ACTION_DIVIDE);
	BIND_ENUM_CONSTANT(ACTION_SET);
	BIND_ENUM_CONSTANT(ACTION_AND);
	BIND_ENUM_CONSTANT(ACTION_OR);
}

Variant PropertyTweaker::evaluate() {
	Variant value = base;
	for(TweakImpl* p_tweak : tweaks)
	{
		value = p_tweak->apply(value);
	}
	return value;
}

Variant PropertyTweaker::set_base(const Variant &p_value) {
	base = p_value;
	return evaluate();
}

class TweakSort
{
public:
	bool operator()(TweakImpl* a, TweakImpl* b)
	{
		if(a->get_priority() < b->get_priority())
		{
			return true;
		}
		if(a->get_priority() > b->get_priority())
		{
			return false;
		}
		return a->get_order() < b->get_order();
	}
};

void PropertyTweaker::recalculate() {
	if(p_owner!=nullptr)
	{
		tweaks.sort_custom<TweakSort>();
		Variant val = evaluate();
		p_owner->set_direct(p_prop, val);
	}
}

Variant ObjectTweaker::set_base(const StringName &p_name, const Variant &p_value) {
	PropertyTweaker* ptr = props.getptr(p_name);
	if(ptr == nullptr)
	{
		return p_value;
	}
	return ptr->set_base(p_value);
}

PropertyTweaker *ObjectTweaker::get_property_tweaker(const StringName &property) {
	PropertyTweaker* p_tweaker = props.getptr(property);
	if(p_tweaker!=nullptr)
	{
		return p_tweaker;
	}
	props.insert(property, PropertyTweaker(p_owner, property));
	return props.getptr(property);
}

void ObjectTweaker::set_owning_object(Object *p_object) {
	p_owner = p_object;
}

Variant ObjectTweaker::get_tweaked(const StringName &property, const Variant &add_to_base) {
	PropertyTweaker* ptr = props.getptr(property);
	if(ptr == nullptr)
	{
		return Variant::evaluate(Variant::Operator::OP_ADD, p_owner->get(property), add_to_base);
	}
	return ptr->get_tweaked(add_to_base);
}

ObjectTweaker::~ObjectTweaker() {
	for(auto& p : props)
	{
		p.value.remove_owner();
	}
}

Variant PropertyTweaker::get_tweaked(const Variant& add_to_base)
{
	Variant value = Variant::evaluate(Variant::Operator::OP_ADD, base, add_to_base);
	for(TweakImpl* p_tweak : tweaks)
	{
		value = p_tweak->apply(value);
	}
	return value;
}

void PropertyTweaker::remove_owner() {
	p_owner = nullptr;
	for(TweakImpl* p_tweak : tweaks)
	{
		p_tweak->set_owning_tweaker(nullptr);
	}
	tweaks.clear();
}

void PropertyTweaker::add_tweak(TweakImpl *p_tweak) {
	p_tweak->set_owning_tweaker(this);
	p_tweak->set_order(tweak_order++);
	tweaks.push_back(p_tweak);
	recalculate();
}

void PropertyTweaker::remove_tweak(TweakImpl *p_tweak) {
	tweaks.erase(p_tweak);
	p_tweak->set_owning_tweaker(nullptr);
	recalculate();
}

PropertyTweaker::PropertyTweaker(Object *p_obj, const StringName &p_property) : p_owner(p_obj), p_prop(p_property)  {
	base = p_owner->get(p_prop);
}

Variant TweakImpl::apply(const Variant &value) const {
	return tweak_value;
}

void TweakImpl::set_owning_tweaker(PropertyTweaker *p_tweaker) {
	p_owner = p_tweaker;
}

void TweakImpl::set_order(int _order) {
	order = _order;
}

void TweakImpl::set_value(const Variant &val) {
	tweak_value = val;
	if(p_owner!=nullptr)
	{
		p_owner->recalculate();
	}
}

void TweakImpl::set_priority(int p)
{
	priority = p;
}

void TweakImpl::set_monitor(TweakMonitor *pM) {
	pMonitor = pM;
}

void TweakImpl::disconnect() {
	if(p_owner)
	{
		p_owner->remove_tweak(this);
		p_owner = nullptr;
	}
}

TweakImpl::~TweakImpl() {
	if(pMonitor)
	{
		pMonitor->set_observer(nullptr);
		memdelete(pMonitor);
	}
	disconnect();
}

Variant TweakAdd::apply(const Variant &value) const {
	return Variant::evaluate(Variant::Operator::OP_ADD, value, tweak_value);
}

Variant TweakSubtract::apply(const Variant &value) const {
	return Variant::evaluate(Variant::Operator::OP_SUBTRACT, value, tweak_value);
}

Variant TweakMultiply::apply(const Variant &value) const {
	return Variant::evaluate(Variant::Operator::OP_MULTIPLY, value, tweak_value);
}

Variant TweakDivide::apply(const Variant &value) const {
	return Variant::evaluate(Variant::Operator::OP_DIVIDE, value, tweak_value);
}

Variant TweakMonitor::apply(const Variant &value) const {
	if(pObserver != nullptr)
	{
		pObserver->set_value(value);
	}
	return value;
}

TweakMonitor::~TweakMonitor() {
	if(pObserver != nullptr)
	{
		pObserver->disconnect();
	}
	disconnect();
}

void TweakMonitor::set_observer(TweakImpl *pTweak) {
	pObserver = pTweak;
}

Variant TweakLogicalAnd::apply(const Variant &value) const {
	return Variant::evaluate(Variant::Operator::OP_AND, value, tweak_value);
}

Variant TweakLogicalOr::apply(const Variant &value) const {
	return Variant::evaluate(Variant::Operator::OP_OR, value, tweak_value);
}
