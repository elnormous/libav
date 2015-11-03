//
//  jsonutils.c
//  avconv
//
//  Created by Elviss Strazdins on 03/11/15.
//  Copyright © 2015 Elviss Strazdins. All rights reserved.
//

#include "jsonutils.h"

int split_json(OptionParseContext *octx, struct json_object *fjson,
               const OptionDef *options,
               const OptionGroupDef *groups, int nb_groups)
{
    int i;
    json_object *current_section;
    
    //init_parse_context(octx, groups, nb_groups);
    
    //json_object_object_foreachC
    json_object_object_foreach(fjson, key, val) {
        
        if (strncmp(key, "global", sizeof("global")) == 0) {
            
            // parse global options
            
            struct json_object* loglevel;
            
            if (json_object_object_get_ex(val, "loglevel", &loglevel)) {
                const char *level = json_object_get_string(loglevel);
                
                opt_loglevel(NULL, "loglevel", level);
            }
        }
        else if (strncmp(key, "input", sizeof("input")) == 0) {
            
            // parse input options
            
            struct array_list *input_array = json_object_get_array(val);
            
            if (input_array) {
                int len = json_object_array_length(val);
                
                for (i = 0; i < len; ++i) {
                    
                    current_section = json_object_array_get_idx(val, i);
                    
                    printf("input_key: %d\n", i);
                }
            }
        }
        else if (strncmp(key, "filter_complex", sizeof("filter_complex")) == 0) {
            
            // parse filter options
            
            struct array_list *filter_array = json_object_get_array(val);
            
            if (filter_array) {
                int len = json_object_array_length(val);
                
                for (i = 0; i < len; ++i) {
                    
                    current_section = json_object_array_get_idx(val, i);
                    
                    printf("filter_array: %d\n", i);
                }
            }
            
        }
        else if (strncmp(key, "output", sizeof("output")) == 0) {
            
            // parse output options
            
            struct array_list *output_array = json_object_get_array(val);
            
            if (output_array) {
                int len = json_object_array_length(val);
                
                for (i = 0; i < len; ++i) {
                    
                    current_section = json_object_array_get_idx(val, i);
                    
                    printf("output_key: %d\n", i);
                }
            }
        }
    }
    
    return 0;
}
