#!/usr/bin/env python

import xmlrpclib
import xml

def to_xml(d):
    dom = xml.dom.minidom.Document()
    upgrade_response = dom.createElement("upgrade_response")
    dom.appendChild(upgrade_response)

    for key, value in sorted(d.items()):
        key_value_element = dom.createElement("key_value_pair")
        upgrade_response.appendChild(key_value_element)

        key_element = dom.createElement("key")
        key_text_node = dom.createTextNode(key)
        key_element.appendChild(key_text_node)
        key_value_element.appendChild(key_element)

        value_element = dom.createElement("value")
        value_text_mode = dom.createTextNode(value)
        value_element.appendChild(value_text_mode)
        key_value_element.appendChild(value_element)

    return dom.toxml()
