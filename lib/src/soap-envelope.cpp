// Copyright Maarten L. Hekkelman, Radboud University 2008-2013.
//        Copyright Maarten L. Hekkelman, 2014-2019
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)

#include <zeep/exception.hpp>
#include <zeep/envelope.hpp>
#include <zeep/xml/xpath.hpp>

namespace zeep {

envelope::envelope()
	: m_request(nullptr)
{
}

envelope::envelope(xml::document& data)
	: m_request(nullptr)
{
	const xml::xpath
		sRequestPath("/Envelope[namespace-uri()='http://schemas.xmlsoap.org/soap/envelope/']/Body[position()=1]/*[position()=1]");
	
	std::list<xml::element*> l = sRequestPath.evaluate<xml::element>(*data.root());
	
	if (l.empty())
		throw zeep::exception("Empty or invalid SOAP envelope passed");
	
	m_request = l.front();
}

xml::element* make_envelope(xml::element* data)
{
	xml::element* env(new xml::element("env:Envelope"));
	env->set_name_space("env", "http://schemas.xmlsoap.org/soap/envelope/");
	xml::element* body(new xml::element("env:Body"));
	env->append(body);
	body->append(data);
	
	return env;
}

xml::element* make_fault(const std::string& what)
{
	xml::element* fault(new xml::element("env:Fault"));
	
	xml::element* faultCode(new xml::element("faultcode"));
	faultCode->content("env:Server");
	fault->append(faultCode);
	
	xml::element* faultString(new xml::element("faultstring"));
	faultString->content(what);
	fault->append(faultString);

	return make_envelope(fault);
}

xml::element* make_fault(const std::exception& ex)
{
	return make_fault(std::string(ex.what()));
}

}

