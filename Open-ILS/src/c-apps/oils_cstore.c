#include "opensrf/osrf_application.h"
#include "opensrf/osrf_settings.h"
#include "opensrf/utils.h"
#include "objson/object.h"
#include "opensrf/log.h"
#include "oils_utils.h"
#include "oils_constants.h"
#include "oils_event.h"
#include <dbi/dbi.h>

#include <string.h>
#include <libxml/globals.h>
#include <libxml/xmlerror.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/debugXML.h>
#include <libxml/xmlmemory.h>
//#include <openils/fieldmapper_lookup.h>

#define OILS_AUTH_CACHE_PRFX "oils_cstore_"
#define MODULENAME "open-ils.cstore"
#define PERSIST_NS "http://open-ils.org/spec/opensrf/IDL/persistance/v1"
#define OBJECT_NS "http://open-ils.org/spec/opensrf/IDL/objects/v1"
#define BASE_NS "http://opensrf.org/spec/IDL/base/v1"

int osrfAppChildInit();
int osrfAppInitialize();

int dispatchCRUDMethod ( osrfMethodContext* ctx );
jsonObject* doCreate ( osrfHash* metadata, jsonObject* params );
jsonObject* doRetrieve ( osrfHash* metadata, jsonObject* params );
jsonObject* doUpdate ( osrfHash* metadata, jsonObject* params );
jsonObject* doDelete ( osrfHash* metadata, jsonObject* params );
jsonObject* oilsMakeJSONFromResult( dbi_result result, osrfHash* meta);

dbi_conn dbhandle; /* our db connection */
xmlDocPtr idlDoc = NULL; // parse and store the IDL here

/* handy NULL json object to have around */
static jsonObject* oilsNULL = NULL;

int osrfAppInitialize() {
	osrfLogInfo(OSRF_LOG_MARK, "Initializing the CStore Server...");
	osrfLogInfo(OSRF_LOG_MARK, "Finding XML file...");

	char * idl_filename = osrf_settings_host_value("/apps/%s/app_settings/IDL", MODULENAME);
	osrfLogInfo(OSRF_LOG_MARK, "Found file:");
	osrfLogInfo(OSRF_LOG_MARK, idl_filename);

	osrfLogInfo(OSRF_LOG_MARK, "Parsing the IDL XML...");
	idlDoc = xmlReadFile( idl_filename, NULL, XML_PARSE_XINCLUDE );
	
	if (!idlDoc) {
		osrfLogError(OSRF_LOG_MARK, "Could not load or parse the IDL XML file!");
		exit(1);
	}

	osrfLogInfo(OSRF_LOG_MARK, "...IDL XML parsed");

	osrfStringArray* global_methods = osrfNewStringArray(1);

	//osrfStringArrayAdd( global_methods, "create" );
	osrfStringArrayAdd( global_methods, "retrieve" );
	//osrfStringArrayAdd( global_methods, "update" );
	//osrfStringArrayAdd( global_methods, "delete" );

	xmlNodePtr docRoot = xmlDocGetRootElement(idlDoc);
	xmlNodePtr kid = docRoot->children;
	while (kid) {
		if (!strcmp( (char*)kid->name, "class" )) {
			int i = 0; 
			char* method_type;
			while ( (method_type = osrfStringArrayGetString(global_methods, i)) ) {

				osrfHash * usrData = osrfNewHash();
				osrfHashSet( usrData, xmlGetProp(kid, "id"), "classname");
				osrfHashSet( usrData, xmlGetNsProp(kid, "tablename", PERSIST_NS), "tablename");
				osrfHashSet( usrData, xmlGetNsProp(kid, "fieldmapper", OBJECT_NS), "fieldmapper");

				osrfHash* links = osrfNewHash();
				osrfHash* fields = osrfNewHash();
				xmlNodePtr _cur = kid->children;
				while (_cur) {
					if (!strcmp( (char*)_cur->name, "fields" )) {
						osrfHash* _tmp = osrfNewHash();

						osrfHashSet(
							_tmp,
							strdup( (char*)xmlGetNsProp(_cur, "array_position", OBJECT_NS) ),
							"array_position"
						);

						osrfHashSet(
							_tmp,
							strdup( (char*)xmlGetNsProp(_cur, "name", BASE_NS) ),
							"name"
						);

						osrfHashSet(
							_tmp,
							strdup( (char*)xmlGetNsProp(_cur, "virtual", BASE_NS) ),
							"virtual"
						);

						osrfHashSet(
							fields,
							_tmp,
							strdup( (char*)xmlGetNsProp(_cur, "name", BASE_NS) )
						);
					}

					if (!strcmp( (char*)_cur->name, "links" )) {
						osrfHash* _tmp = osrfNewHash();

						osrfHashSet(
							_tmp,
							strdup( (char*)xmlGetNsProp(_cur, "reltype", BASE_NS) ),
							"reltype"
						);

						osrfHashSet(
							_tmp,
							strdup( (char*)xmlGetNsProp(_cur, "field", BASE_NS) ),
							"field"
						);

						osrfHashSet(
							_tmp,
							strdup( (char*)xmlGetNsProp(_cur, "key", BASE_NS) ),
							"key"
						);

						osrfHashSet(
							_tmp,
							strdup( (char*)xmlGetNsProp(_cur, "class", BASE_NS) ),
							"class"
						);

						osrfStringArray* map = osrfNewStringArray(0);
						char* map_list = strdup((char*)xmlGetNsProp(_cur, "map", BASE_NS));
						if (strlen(map_list) > 0) {
							char* _map_class = strtok(map_list, " ");
							osrfStringArrayAdd(map, strdup(_map_class));
							while ((_map_class = strtok(NULL, " "))) {
								osrfStringArrayAdd(map, strdup(_map_class));
							}
						}
						osrfHashSet( _tmp, map, "map");

						osrfHashSet(
							links,
							_tmp,
							strdup( (char*)xmlGetNsProp(_cur, "field", BASE_NS) )
						);
					}

					_cur = _cur->next;
				}


				char* _fm = strdup( (char*)osrfHashGet(usrData, "fieldmapper") );
				char* part = strtok(_fm, ":");

				growing_buffer* method_name =  buffer_init(64);
				buffer_fadd(method_name, "%s.direct.%s", MODULENAME, part);

				while ((part = strtok(NULL, ":"))) {
					buffer_fadd(method_name, ".%s", part);
				}
				buffer_fadd(method_name, ".%s", method_type);

				char* method = buffer_data(method_name);
				buffer_free(method_name);

				osrfHashSet( usrData, method, "methodname" );
				osrfHashSet( usrData, strdup(method_type), "methodtype" );

				osrfAppRegisterExtendedMethod(
					MODULENAME,
					method,
					"dispatchCRUDMethod",
					"",
					1,
					0,
					(void*)usrData
				);

				i++;
			}
		}
		kid = kid->next;
	}

	return 0;
}

/**
 * Connects to the database 
 */
int osrfAppChildInit() {

	dbi_initialize(NULL);

	char* driver	= osrf_settings_host_value("/apps/%s/app_settings/driver", MODULENAME);
	char* user	= osrf_settings_host_value("/apps/%s/app_settings/database/user", MODULENAME);
	char* host	= osrf_settings_host_value("/apps/%s/app_settings/database/host", MODULENAME);
	char* port	= osrf_settings_host_value("/apps/%s/app_settings/database/port", MODULENAME);
	char* db	= osrf_settings_host_value("/apps/%s/app_settings/database/db", MODULENAME);
	char* pw	= osrf_settings_host_value("/apps/%s/app_settings/database/pw", MODULENAME);

	dbhandle = dbi_conn_new(driver);

	if(!dbhandle) {
		osrfLogError(OSRF_LOG_MARK, "Error creating database driver %s", driver);
		return -1;
	}

	osrfLogInfo(OSRF_LOG_MARK, "oils_fetch connecting to database.  host=%s, "
		"port=%s, user=%s, pw=%s, db=%s", host, port, user, pw, db );

	if(host) dbi_conn_set_option(dbhandle, "host", host );
	if(port) dbi_conn_set_option_numeric( dbhandle, "port", atoi(port) );
	if(user) dbi_conn_set_option(dbhandle, "username", user);
	if(pw) dbi_conn_set_option(dbhandle, "password", pw );
	if(db) dbi_conn_set_option(dbhandle, "dbname", db );

	free(user);
	free(host);
	free(port);
	free(db);
	free(pw);

	if (dbi_conn_connect(dbhandle) < 0) {
		const char* err;
		dbi_conn_error(dbhandle, &err);
		osrfLogError( OSRF_LOG_MARK, "Error connecting to database: %s", err);
		return -1;
	}

	osrfLogInfo(OSRF_LOG_MARK, "%s successfully connected to the database", MODULENAME);

	return 0;
}


int dispatchCRUDMethod ( osrfMethodContext* ctx ) {
	OSRF_METHOD_VERIFY_CONTEXT(ctx);

	osrfHash* meta = (osrfHash*) ctx->method->userData;

	jsonObject * obj = NULL;
	if (!strcmp( (char*)osrfHashGet(meta, "methodtype"), "create"))
		obj = doCreate(meta, ctx->params);

	if (!strcmp( (char*)osrfHashGet(meta, "methodtype"), "retrieve"))
		obj = doRetrieve(meta, ctx->params);

	if (!strcmp( (char*)osrfHashGet(meta, "methodtype"), "update"))
		obj = doUpdate(meta, ctx->params);

	if (!strcmp( (char*)osrfHashGet(meta, "methodtype"), "delete"))
		obj = doDelete(meta, ctx->params);

	osrfAppRespondComplete( ctx, obj );

	jsonObjectFree(obj);

	return 0;
}

jsonObject* doCreate( osrfHash* meta, jsonObject* params ) { return NULL; }

jsonObject* doRetrieve( osrfHash* meta, jsonObject* params ) {

	char* id	= jsonObjectToSimpleString(jsonObjectGetIndex(params, 0));

	osrfLogDebug(
		OSRF_LOG_MARK,
		"%s retrieving %s object with id %s",
		MODULENAME,
		osrfHashGet(meta, "fieldmapper"),
		id
	);



	growing_buffer* sql_buf = buffer_init(128);
	buffer_fadd(
		sql_buf,
		"SELECT * FROM %s WHERE %s = %d;",
		osrfHashGet(meta, "tablename"),
		xmlGetNsProp( (xmlNode*)osrfHashGet(meta, "fields"), "primary", "http://open-ils.org/spec/opensrf/IDL/persistance/v1" ),
		atoi(id)
	);

	char* sql = buffer_data(sql_buf);
	buffer_free(sql_buf);
	
	osrfLogDebug(OSRF_LOG_MARK, "%s SQL =  %s", MODULENAME, sql);

	dbi_result result = dbi_conn_query(dbhandle, sql);

	jsonObject* obj = NULL;
	if(result) {

		/* there should be one row at the most  */
		if (!dbi_result_first_row(result)) {
			osrfLogDebug(OSRF_LOG_MARK, "%s: Error retrieving %s with key %s", MODULENAME, osrfHashGet(meta, "fieldmapper"), id);
			dbi_result_free(result); 
			return oilsNULL;
		}

		/* JSONify the result */
		obj = oilsMakeJSONFromResult( result, meta );

		/* clean up the query */
		dbi_result_free(result); 

	} else {
		osrfLogDebug(OSRF_LOG_MARK, "%s returned no results for query %s", MODULENAME, sql);
	}

	free(id);

	return obj;
}


jsonObject* doUpdate( osrfHash* meta, jsonObject* params ) { return NULL; }

jsonObject* doDelete( osrfHash* meta, jsonObject* params ) { return NULL; }


jsonObject* oilsMakeJSONFromResult( dbi_result result, osrfHash* meta) {
	if(!(result && meta)) return NULL;

	jsonObject* object = jsonParseString("[]");
	jsonObjectSetClass(object, osrfHashGet(meta, "classname"));

	osrfHash* fields = osrfHashGet(meta, "fields");

	osrfHash* _f;
	int attr;
	int fmIndex;
	int columnIndex = 1;
	unsigned short type;
	const char* columnName;

	/* cycle through the column list */
	while( (columnName = dbi_result_get_field_name(result, columnIndex++)) ) {

		fmIndex = -1; // reset the position
		
		/* determine the field type and storage attributes */
		type = dbi_result_get_field_type(result, columnName);
		attr = dbi_result_get_field_attribs(result, columnName);

		/* fetch the fieldmapper index */
		if( (_f = osrfHashGet(fields, (char*)columnName)) ) {
			if ( !(strcmp( osrfHashGet(_f, "virtual"), "true" )) ) continue;
			if ( !(stringisnum( osrfHashGet(_f, "array_position") )) ) continue;
			fmIndex = atoi(osrfHashGet(_f, "array_position"));
		}

		switch( type ) {

			case DBI_TYPE_INTEGER :

				if( attr & DBI_INTEGER_SIZE8 ) 
					jsonObjectSetIndex( object, fmIndex, 
						jsonNewNumberObject(dbi_result_get_longlong(result, columnName)));
				else 
					jsonObjectSetIndex( object, fmIndex, 
						jsonNewNumberObject(dbi_result_get_long(result, columnName)));

				break;

			case DBI_TYPE_DECIMAL :
				jsonObjectSetIndex( object, fmIndex, 
						jsonNewNumberObject(dbi_result_get_double(result, columnName)));
				break;

			case DBI_TYPE_STRING :
				jsonObjectSetIndex( object, fmIndex, 
					jsonNewObject(dbi_result_get_string(result, columnName)));
				break;

			case DBI_TYPE_DATETIME :
				jsonObjectSetIndex( object, fmIndex, 
					jsonNewNumberObject(dbi_result_get_datetime(result, columnName)));
				break;

			case DBI_TYPE_BINARY :
				osrfLogError( OSRF_LOG_MARK, 
					"Can't do binary at column %s : index %d", columnName, columnIndex - 1);
		}
	}

	return object;
}




