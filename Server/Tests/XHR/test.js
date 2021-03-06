/*
* This file is part of Wakanda software, licensed by 4D under
*  (i) the GNU General Public License version 3 (GNU GPL v3), or
*  (ii) the Affero General Public License version 3 (AGPL v3) or
*  (iii) a commercial license.
* This file remains the exclusive property of 4D and/or its licensors
* and is protected by national and international legislations.
* In any event, Licensee's compliance with the terms and conditions
* of the applicable license constitutes a prerequisite to any use of this file.
* Except as otherwise expressly stated in the applicable license,
* such license does not include any other license or rights on this file,
* 4D's and/or its licensors' trademarks and/or other proprietary rights.
* Consequently, no title, copyright or other proprietary rights
* other than those specified in the applicable license is granted.
*/
var testCase = {
	name: "XHRBasicTest",
	
	_should: {
        ignore: {
            testConstructorHasSetDefaultProxyMethod: true // Don't know yet if it's still planned or not...
        }
    },
	
	// INTERFACE
	
    testObjectExists: function () {
        Y.Assert.areSame("object", typeof XMLHttpRequest);
    },
	testBasicInstanciation: function () {
        var myXHR = new XMLHttpRequest();
        Y.Assert.areSame("object", typeof myXHR);
    },
	testConstructorHasSetDefaultProxyMethod: function () {
        Y.Assert.areSame("function", typeof XMLHttpRequest.setDefaultProxy);
    },
	testInstanceHasGetResponseHeaderMethod: function () {
        var myXHR = new XMLHttpRequest();
        Y.Assert.areSame("function", typeof myXHR.getResponseHeader);
    },
	testInstanceHasGetAllResponseHeadersMethod: function () {
        var myXHR = new XMLHttpRequest();
        Y.Assert.areSame("function", typeof myXHR.getAllResponseHeaders);
    },
	testInstanceHasOpenMethod: function () {
        var myXHR = new XMLHttpRequest();
        Y.Assert.areSame("function", typeof myXHR.open);
    },
	testInstanceHasSendMethod: function () {
        var myXHR = new XMLHttpRequest();
        Y.Assert.areSame("function", typeof myXHR.send);
    },
	testInstanceHasSetRequestHeaderMethod: function () {
        var myXHR = new XMLHttpRequest();
        Y.Assert.areSame("function", typeof myXHR.setRequestHeader);
    },
	
	// GET
	testInstanceBasic200GETRequest: function () {
        var result = null;
        var myXHR = new XMLHttpRequest();
        myXHR.open("GET", "http://127.0.0.1:8081/testSimple", false);
        myXHR.send();
        result = myXHR.status;
        Y.Assert.areSame(200, result);
    },
	testInstanceBasic404GETRequest: function () {
        var result = null;
		var myXHR = new XMLHttpRequest();
		myXHR.open("GET", "http://127.0.0.1:8081/foobar", false);
		myXHR.send();
		result = myXHR.status;
		Y.Assert.areSame(404, result);
    },
	testInstanceBasicFailedGETRequest: function () {
        var invalid = false;
		var myXHR = new XMLHttpRequest();
		try {
			myXHR.open("GET", "http://foo.bar.baz/42", false);
			myXHR.send();
		} catch (e) {
			invalid = true;
		}
       	Y.Assert.areSame(true, invalid);
    },
	testInstanceEchoedGETRequest: function () {
        var result = null;
        var myXHR = new XMLHttpRequest();
		myXHR.open("GET", "http://127.0.0.1:8081/testEcho/foo?bar=baz", false);
		myXHR.send();
		result = JSON.parse(myXHR.responseText);
		Y.Assert.areSame("object", typeof result);
		Y.ObjectAssert.hasKeys([
			"url", 
			"rawURL", 
			"urlPath", 
			"urlQuery", 
			"host",
			"method",
			"version",
			"user",
			"password",
			"requestLine",
			"headers",
			"body"
		], result);
		Y.ObjectAssert.hasKeys([
			"ACCEPT",
			"HOST"
		], result.headers);
		Y.Assert.areSame("/testEcho/foo?bar=baz", result.url);
		Y.Assert.areSame("/testEcho/foo?bar=baz", result.rawURL);
		Y.Assert.areSame("/testEcho/foo", result.urlPath);
		Y.Assert.areSame("bar=baz", result.urlQuery);
		Y.Assert.areSame("127.0.0.1:8081", result.host);
		Y.Assert.areSame("GET", result.method);
		Y.Assert.areSame("HTTP/1.1", result.version);
		Y.Assert.areSame("", result.user);
		Y.Assert.areSame("", result.password);
		Y.Assert.areSame("GET /testEcho/foo?bar=baz HTTP/1.1", result.requestLine);
		Y.Assert.areSame("*" + "/" + "*", result.headers.ACCEPT);
		Y.Assert.areSame("127.0.0.1:8081", result.headers.HOST);
		Y.Assert.isObject(result.body);
		Y.Assert.areSame(0, result.body.size);
		Y.Assert.areSame("application/octet-stream", result.body.type);
    },
	
	// POST
	
	testInstanceBasic405POSTRequest: function () {
        var result = null;
        var myXHR = new XMLHttpRequest();
		myXHR.open("POST", "http://127.0.0.1:8081", false);
		myXHR.send();
		result = myXHR.status;
		Y.Assert.areSame(405, result);
    },
	testInstanceBasicFailedPOSTRequest: function () {
        var invalid = false;
		var myXHR = new XMLHttpRequest();
		try {
			myXHR.open("POST", "http://foo.bar.baz/42", false);
			myXHR.send();
		} catch (e) {
			invalid = true;
		}
        Y.Assert.areSame(true, invalid);
    },
	testInstanceEchoedPOSTRequest: function () {
        var result = null;
		var myXHR = new XMLHttpRequest();
		myXHR.open("POST", "http://127.0.0.1:8081/testEcho/foo?bar= baz", false);
		myXHR.send("{\"foo\": 'bar baz',\n\t\"42\": 42}");
		result = JSON.parse(myXHR.responseText);
		Y.Assert.areSame("object", typeof result);
		Y.ObjectAssert.hasKeys([
			"url", 
			"rawURL", 
			"urlPath", 
			"urlQuery", 
			"host",
			"method",
			"version",
			"user",
			"password",
			"requestLine",
			"headers",
			"body"
		], result);
		Y.ObjectAssert.hasKeys([
			"ACCEPT",
			"HOST"
		], result.headers);
		Y.Assert.areSame("/testEcho/foo?bar= baz", result.url);
		Y.Assert.areSame("/testEcho/foo?bar= baz", result.rawURL);
		Y.Assert.areSame("/testEcho/foo", result.urlPath);
		Y.Assert.areSame("bar= baz", result.urlQuery);
		Y.Assert.areSame("127.0.0.1:8081", result.host);
		Y.Assert.areSame("POST", result.method);
		Y.Assert.areSame("HTTP/1.1", result.version);
		Y.Assert.areSame("", result.user);
		Y.Assert.areSame("", result.password);
		Y.Assert.areSame("POST /testEcho/foo?bar= baz HTTP/1.1", result.requestLine);
		Y.Assert.areSame("*" + "/" + "*", result.headers.ACCEPT);
		Y.Assert.areSame("127.0.0.1:8081", result.headers.HOST);
		Y.Assert.areSame("{\"foo\": 'bar baz',\n\t\"42\": 42}", result.body);
    },

	testHttps: function () {

		xhr = new XMLHttpRequest();
		xhr.open('GET', "https://www.google.fr/");
		xhr.send();
		var status = xhr.status;
	
		Y.Assert.areSame(200, status);
	}
};
